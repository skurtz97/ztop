/**
 * @file ztop.c
 * @brief What z/OS has where: storage by subpool, the system, and jobs.
 *
 * One-shot reports for scripts and a full-screen view for people.  The
 * view is built on libtui, which knows nothing about z/OS: ztop supplies
 * the model, the messages and the tables.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

#include "sys.h"
#include "tui.h"
#include "vsm.h"

#define ZTOP_VERSION "0.3"
#define WORK_SIZE (1024u * 1024u)

#if defined(__MVS__)
/* HLASM upcases the ESD name of the CSECT, so the assembler side is
 * exported as ZVSLIST whatever case the source is written in. */
extern int zvslist(void *) __asm__("ZVSLIST");
#pragma linkage(zvslist, OS)

/* The 31-bit request block.  R13 is saved by the stub, so the save area
 * has to live in storage the C compiler gives us, not in the read-only
 * CSECT. */
struct zreq {
    int area;
    unsigned len;
    void *wk;
    int rc;
    void *self;
    unsigned long long save[18];
    int space;
};
_Static_assert(offsetof(struct zreq, self) == 16, "zreq.self");
_Static_assert(offsetof(struct zreq, save) == 24, "zreq.save");
_Static_assert(offsetof(struct zreq, space) == 168, "zreq.space");

static unsigned char work[WORK_SIZE] __attribute__((aligned(8)));
#endif

static const char *const area_name[] = {"csa", "sqa", "lsqa", "pvt"};
static const char *const area_upper[] = {"CSA", "SQA", "LSQA", "PVT"};

/** @brief A byte count in K, M or G, whichever fits best. */
static void human(uint64_t n, char *buf, size_t len) {
    if (n >= (1ULL << 30))
        (void)snprintf(buf, len, "%.1fG", (double)n / (double)(1ULL << 30));
    else if (n >= (1ULL << 20))
        (void)snprintf(buf, len, "%.1fM", (double)n / (double)(1ULL << 20));
    else if (n >= (1ULL << 10))
        (void)snprintf(buf, len, "%.1fK", (double)n / (double)(1ULL << 10));
    else
        (void)snprintf(buf, len, "%llu", (unsigned long long)n);
}

#ifdef __MVS__
static void hexdump(const char *tag, const unsigned char *p, unsigned int len,
                    unsigned int max) {
    unsigned int i, j;

    if (len > max)
        len = max;
    for (i = 0; i < len; i += 16) {
        (void)fprintf(stderr, "%s %04X:", tag, i);
        for (j = 0; j < 16 && i + j < len; j++)
            (void)fprintf(stderr, " %02X", p[i + j]);
        (void)fputc('\n', stderr);
    }
}

static void dump_work(const struct vsm_info *v, const unsigned char *workp,
                      const unsigned char *data, unsigned int len, int rc) {
    (void)fprintf(stderr, "ztop: VSMLIST %s %s rc=%d data=%08X len=%u\n",
                  area_name[v->area - 1],
                  v->space == VSM_UNALLOC ? "UNALLOC" : "FREE", rc,
                  (unsigned int)(uintptr_t)data, len);
    hexdump("work", workp, 32, 32);
    if (data != NULL && len > 0)
        hexdump("data", data, len, 256);
}

static bool fetch_one(struct vsm_info *v, int area, int space, bool dump) {
    int calls = 0;
    const unsigned char *data;
    unsigned int len;

    vsm_reset(v, (unsigned int)area, (unsigned int)space);
    memset(work, 0, 4); /* the return code must start at 0 */
    for (;;) {
        struct zreq req = {0};
        bool ok;

        if (++calls > 64) {
            (void)snprintf(v->err, sizeof v->err,
                           "VSMLIST did not finish after 64 calls");
            return false;
        }
        req.area = area;
        req.space = space;
        req.len = WORK_SIZE;
        req.wk = work;
        req.rc = -1;
        (void)zvslist(&req);
        if (req.rc != 0 && req.rc != 4) {
            (void)snprintf(v->err, sizeof v->err, "VSMLIST returned %d",
                           req.rc);
            return false;
        }
        if (!vsm_work_data(work, WORK_SIZE, &data, &len)) {
            (void)snprintf(v->err, sizeof v->err,
                           "VSMLIST work area has an unusable data pointer");
            return false;
        }
        if (dump)
            dump_work(v, work, data, len, req.rc);
        ok = space == VSM_UNALLOC ? vsm_parse_unalloc(v, data, len)
                                  : vsm_parse_alloc(v, data, len);
        if (!ok)
            return false;
        if (req.rc == 0)
            return true;
        /* rc 4: the service wants the same request and work area again. */
    }
}
#else
static bool fetch_one(struct vsm_info *v, int area, int space, bool dump) {
    (void)area;
    (void)space;
    (void)dump;
    (void)snprintf(v->err, sizeof v->err, "VSMLIST is only available on z/OS");
    return false;
}
#endif

static bool collect_area(struct vsm_info *v, int area, bool dump) {
    if (!fetch_one(v, area, VSM_FREE, dump))
        return false;
    if (area == VSM_CSA || area == VSM_PVT) {
        struct vsm_info u;

        if (fetch_one(&u, area, VSM_UNALLOC, dump)) {
            v->unalloc = u.unalloc;
            v->unalloc_blocks = u.unalloc_blocks;
            v->largest_unalloc = u.largest_unalloc;
            v->nregion = u.nregion;
            memcpy(v->region, u.region, sizeof v->region);
            v->unalloc_valid = true;
        } else {
            (void)snprintf(v->err, sizeof v->err, "%s", u.err);
        }
    }
    return true;
}

/** @brief Fraction of the mapped area in use, or -1 when there is no total. */
static int pct(const struct vsm_info *v) {
    uint64_t total;

    if (!v->unalloc_valid || v->area != VSM_CSA)
        return -1;
    total = v->foot + v->unalloc;
    if (total == 0)
        return 0;
    return (int)((v->used * 100u + total / 2) / total);
}

static void print_area(const struct vsm_info *v) {
    const char *name = area_name[v->area - 1];
    bool pvt = v->area == VSM_PVT;
    int p = pct(v);
    int i;

    (void)printf("area %s: used %llu free %llu mapped %llu blocks %u", name,
                 (unsigned long long)v->used, (unsigned long long)v->free,
                 (unsigned long long)v->foot, v->blocks);
    if (v->area == VSM_CSA && v->unalloc_valid)
        (void)printf(" unallocated %llu total %llu use %d%%",
                     (unsigned long long)v->unalloc,
                     (unsigned long long)(v->foot + v->unalloc), p);
    if (pvt)
        (void)printf(" tcbs %u", v->tcbs);
    (void)printf("\n");

    if (pvt)
        (void)printf("%3s %3s %8s %5s %10s %10s %10s %10s %8s\n", "sp", "key",
                     "tcb", "blks", "used", "free", "mapped", "largest",
                     "addr");
    else
        (void)printf("%3s %3s %6s %10s %10s %10s %10s %17s\n", "sp", "key",
                     "blks", "used", "free", "mapped", "largest", "address");

    for (i = 0; i < v->nsub; i++) {
        const struct vsm_subpool *s = &v->sub[i];
        char addr[32];

        if (s->blocks == 0)
            (void)snprintf(addr, sizeof addr, "-");
        else if (pvt)
            (void)snprintf(addr, sizeof addr, "%08X", (unsigned int)s->lo);
        else
            (void)snprintf(addr, sizeof addr, "%08X-%08X", (unsigned int)s->lo,
                           (unsigned int)s->hi);
        if (pvt)
            (void)printf("%3u %3u %08X %5u %10llu %10llu %10llu %10llu %s\n",
                         s->id, s->key, (unsigned int)s->tcb, s->blocks,
                         (unsigned long long)s->used,
                         (unsigned long long)s->free,
                         (unsigned long long)s->foot,
                         (unsigned long long)s->largest, addr);
        else
            (void)printf("%3u %3u %6u %10llu %10llu %10llu %10llu %s\n", s->id,
                         s->key, s->blocks, (unsigned long long)s->used,
                         (unsigned long long)s->free,
                         (unsigned long long)s->foot,
                         (unsigned long long)s->largest, addr);
    }
    if (v->truncated)
        (void)printf("(subpool list truncated at %d rows)\n", VSM_MAX_SUB);

    if (v->unalloc_valid) {
        (void)printf("unallocated %llu bytes in %u blocks, largest %llu",
                     (unsigned long long)v->unalloc, v->unalloc_blocks,
                     (unsigned long long)v->largest_unalloc);
        for (i = 0; i < v->nregion && i < VSM_MAX_REGION; i++)
            (void)printf("%s %08X+%08X", i == 0 ? " regions" : ",",
                         (unsigned int)v->region[i].addr,
                         (unsigned int)v->region[i].len);
        (void)printf("\n");
    }
    if (v->err[0] != '\0')
        (void)printf("warning: %s\n", v->err);
}

static void print_system(const struct zsys *z) {
    (void)printf(
        "system %s  host %s  os %s  machine %s  address spaces %d/%u\n",
        z->sysname, z->host, z->os, z->machine, z->njobs, z->slots);
    if (z->ipl_ok)
        (void)printf("ipl %04u-%02u-%02u %02u:%02u:%02u  real storage %llu K\n",
                     z->ipl.year, z->ipl.month, z->ipl.day, z->ipl.hour,
                     z->ipl.minute, z->ipl.second,
                     (unsigned long long)z->real_k);
}

static void print_page(const struct zpage *p) {
    (void)printf("aux storage  common %s%s  plpa %s%s  local %s  vio %s"
                 ", journal %u\n",
                 p->common_pds ? "yes" : "no", p->common_full ? " full" : "",
                 p->plpa_pds ? "yes" : "no", p->plpa_full ? " full" : "",
                 p->local_paging ? "on" : "off",
                 p->vio_journaling ? "on" : "off", p->vio_ci);
}

static void print_jobs(const struct zsys *z) {
    int i;

    (void)printf("%4s %-8s %8s\n", "asid", "job", "ascb");
    for (i = 0; i < z->njobs; i++)
        (void)printf("%4u %-8s %08X\n", z->job[i].asid, z->job[i].name,
                     (unsigned int)z->job[i].ascb);
}

struct app {
    struct vsm_info info[4];
    bool have[4];
    char err[4][160];
    struct zsys sys;
    bool have_sys;
    char sys_err[160];
    struct zpage page;
    bool have_page;
    char page_err[160];
    int interval;
    int collect_ms;
    bool paused;
    bool help;
    int tab;
    int sub;
    struct tui_table storage;
    struct tui_table jobs;
    struct tui_app *tapp;
    char host[65];
    int sub_sort, sub_dir;
    int job_sort, job_dir;
    int sub_order[VSM_MAX_SUB];
    int job_order[ZJOB_MAX];
    bool popup;
};

enum { SS_SP, SS_KEY, SS_BLOCKS, SS_USED, SS_FREE, SS_LARGEST, SS_COUNT };
enum { JS_ASID, JS_JOB, JS_ADDR, JS_COUNT };

static const char *const sub_sort_names[SS_COUNT] = {
    "sp", "key", "blocks", "used", "free", "largest",
};
static const char *const job_sort_names[JS_COUNT] = {"asid", "job", "ascb"};

/* qsort has no context argument; these are set right before each call and
 * the application is single-threaded. */
static const struct vsm_info *sort_info;
static const struct zsys *sort_sys;
static int sort_key, sort_dir;

static int cmp_u64(uint64_t a, uint64_t b) {
    return (a > b) - (a < b);
}
static int cmp_u32(uint32_t a, uint32_t b) {
    return (a > b) - (a < b);
}

static int cmp_sub(const void *pa, const void *pb) {
    int ia = *(const int *)pa;
    int ib = *(const int *)pb;
    const struct vsm_subpool *a = &sort_info->sub[ia];
    const struct vsm_subpool *b = &sort_info->sub[ib];
    int r = 0;

    switch (sort_key) {
    case SS_SP:
        r = cmp_u32(a->id, b->id);
        break;
    case SS_KEY:
        r = cmp_u32(a->key, b->key);
        break;
    case SS_BLOCKS:
        r = cmp_u32(a->blocks, b->blocks);
        break;
    case SS_USED:
        r = cmp_u64(a->used, b->used);
        break;
    case SS_FREE:
        r = cmp_u64(a->free, b->free);
        break;
    default:
        r = cmp_u64(a->largest, b->largest);
        break;
    }
    return r != 0 ? sort_dir * r : ia - ib;
}

static int cmp_job(const void *pa, const void *pb) {
    int ia = *(const int *)pa;
    int ib = *(const int *)pb;
    const struct zjob *a = &sort_sys->job[ia];
    const struct zjob *b = &sort_sys->job[ib];
    int r = 0;

    switch (sort_key) {
    case JS_ASID:
        r = cmp_u32(a->asid, b->asid);
        break;
    case JS_JOB:
        r = strcmp(a->name, b->name);
        break;
    default:
        r = cmp_u32(a->ascb, b->ascb);
        break;
    }
    return r != 0 ? sort_dir * r : ia - ib;
}

static const struct tui_col storage_cols[] = {
    {"SP", 3, true},       {"KY", 3, true},        {"BLKS", 8, true},
    {"USED", 10, true},    {"FREE", 10, true},     {"MAPPED", 10, true},
    {"LARGEST", 10, true}, {"ADDRESS", 17, false},
};

static const struct tui_col pvt_cols[] = {
    {"SP", 3, true},       {"KY", 3, true},    {"BLKS", 7, true},
    {"USED", 10, true},    {"FREE", 10, true}, {"MAPPED", 10, true},
    {"LARGEST", 10, true}, {"TCB", 9, false},
};

static const struct tui_col job_cols[] = {
    {"ASID", 6, true},
    {"JOB", 9, false},
    {"ASCB", 10, false},
};

static void storage_cell(void *ctx, int row, int col, char *buf, size_t len) {
    const struct app *a = ctx;
    int i = row < a->info[a->sub].nsub ? a->sub_order[row] : row;
    const struct vsm_subpool *s = &a->info[a->sub].sub[i];
    char u[16], f[16], m[16], l[16];

    switch (col) {
    case 0:
        (void)snprintf(buf, len, "%u", s->id);
        break;
    case 1:
        (void)snprintf(buf, len, "%u", s->key);
        break;
    case 2:
        (void)snprintf(buf, len, "%u", s->blocks);
        break;
    case 3:
        human(s->used, u, sizeof u);
        (void)snprintf(buf, len, "%s", u);
        break;
    case 4:
        human(s->free, f, sizeof f);
        (void)snprintf(buf, len, "%s", f);
        break;
    case 5:
        human(s->foot, m, sizeof m);
        (void)snprintf(buf, len, "%s", m);
        break;
    case 6:
        human(s->largest, l, sizeof l);
        (void)snprintf(buf, len, "%s", l);
        break;
    default:
        if (a->sub == 3)
            (void)snprintf(buf, len, "%08X", (unsigned int)s->tcb);
        else if (s->blocks == 0)
            (void)snprintf(buf, len, "-");
        else
            (void)snprintf(buf, len, "%08X-%08X", (unsigned int)s->lo,
                           (unsigned int)s->hi);
        break;
    }
}

static void job_cell(void *ctx, int row, int col, char *buf, size_t len) {
    const struct app *a = ctx;
    int i = row < a->sys.njobs ? a->job_order[row] : row;
    const struct zjob *j = &a->sys.job[i];

    switch (col) {
    case 0:
        (void)snprintf(buf, len, "%u", j->asid);
        break;
    case 1:
        (void)snprintf(buf, len, "%s", j->name);
        break;
    default:
        (void)snprintf(buf, len, "%08X", (unsigned int)j->ascb);
        break;
    }
}

static void sys_read(struct app *a) {
    struct zsys z;
    struct utsname u;

    (void)memcpy(&z, &a->sys, sizeof z);
    if (!zsys_read(&z)) {
        (void)snprintf(a->sys_err, sizeof a->sys_err,
                       "CVT/ASVT are only available on z/OS");
        a->have_sys = false;
        return;
    }
    if (uname(&u) == 0) {
        (void)snprintf(z.os, sizeof z.os, "%s %s", u.sysname, u.release);
        (void)snprintf(z.machine, sizeof z.machine, "%s", u.machine);
    }
    (void)snprintf(z.host, sizeof z.host, "%s", a->host);
    a->sys = z;
    a->have_sys = true;
    a->sys_err[0] = '\0';
}

static void collect(struct app *a) {
    struct timespec t0, t1;
    int i;

    (void)clock_gettime(CLOCK_MONOTONIC, &t0);
    for (i = 0; i < 4; i++) {
        struct vsm_info v;

        if (collect_area(&v, i + 1, false)) {
            a->info[i] = v;
            a->have[i] = true;
            a->err[i][0] = '\0';
        } else {
            (void)snprintf(a->err[i], sizeof a->err[i], "%s", v.err);
        }
    }
    sys_read(a);
    a->have_page = zpage_read(&a->page);
    if (!a->have_page)
        (void)snprintf(a->page_err, sizeof a->page_err,
                       "ASMVT is only available on z/OS");
    (void)clock_gettime(CLOCK_MONOTONIC, &t1);
    a->collect_ms = (int)((t1.tv_sec - t0.tv_sec) * 1000 +
                          (t1.tv_nsec - t0.tv_nsec) / 1000000);
}

static void draw_header(struct tui *t, const struct app *a) {
    char now[16];
    time_t clock_now = time(NULL);
    struct tm tm;

    (void)localtime_r(&clock_now, &tm);
    (void)strftime(now, sizeof now, "%H:%M:%S", &tm);
    tui_fill(t, 0, 0, t->w, 1, ' ', TUI_TITLE);
    tui_printf(t, 1, 0, t->w - 30, TUI_TITLE, " ztop %s  %s  %s  collect %dms ",
               ZTOP_VERSION, a->host, now, a->collect_ms);
    if (a->paused)
        tui_put(t, t->w - 9, 0, " PAUSED ", 8, TUI_WARN);
}

static void draw_tabs(struct tui *t, const struct app *a) {
    static const char *const labels[4] = {"1 STORAGE", "2 SYSTEM", "3 JOBS",
                                          "4 PAGING"};
    int x = tui_tabs_draw(t, 0, 1, labels, 4, a->tab);

    tui_printf(t, x + 1, 1, t->w - x - 2, TUI_LABEL, "refresh %ds  +/-",
               a->interval);
}

static void order_storage(struct app *a) {
    const struct vsm_info *v = &a->info[a->sub];

    for (int i = 0; i < v->nsub; i++)
        a->sub_order[i] = i;
    sort_info = v;
    sort_key = a->sub_sort;
    sort_dir = a->sub_dir;
    qsort(a->sub_order, (size_t)v->nsub, sizeof a->sub_order[0], cmp_sub);
}

static void draw_storage(struct tui *t, struct app *a) {
    const struct vsm_info *v = &a->info[a->sub];
    tui_rect r = {0, 5, t->w, t->h - 7};
    char used[16], freed[16], mapped[16], total[16], pcbuf[8];
    char head[24];
    struct tui_col cols[8];
    static const int col_of[SS_COUNT] = {0, 1, 2, 3, 4, 6};
    int p = pct(v);

    order_storage(a);
    (void)memcpy(cols, a->sub == 3 ? pvt_cols : storage_cols, sizeof cols);
    (void)snprintf(head, sizeof head, "%s %s", sub_sort_names[a->sub_sort],
                   a->sub_dir < 0 ? "^" : "v");
    cols[col_of[a->sub_sort]].title = head;
    a->storage.cols = cols;
    tui_printf(t, 1, 3, 6, a->sub == 0 ? TUI_TITLE : TUI_LABEL, " CSA ");
    tui_printf(t, 7, 3, 6, a->sub == 1 ? TUI_TITLE : TUI_LABEL, " SQA ");
    tui_printf(t, 13, 3, 6, a->sub == 2 ? TUI_TITLE : TUI_LABEL, " LSQA ");
    tui_printf(t, 20, 3, 6, a->sub == 3 ? TUI_TITLE : TUI_LABEL, " PVT ");

    if (!a->have[a->sub]) {
        tui_printf(t, 1, 6, t->w - 2, TUI_BAD, "%s: %s", area_upper[a->sub],
                   a->err[a->sub][0] ? a->err[a->sub] : "no data");
        return;
    }
    human(v->used, used, sizeof used);
    human(v->free, freed, sizeof freed);
    human(v->foot, mapped, sizeof mapped);
    if (v->area == VSM_CSA && v->unalloc_valid) {
        human(v->foot + v->unalloc, total, sizeof total);
        (void)snprintf(pcbuf, sizeof pcbuf, "%d%%", p);
    } else {
        (void)snprintf(total, sizeof total, "-");
        (void)snprintf(pcbuf, sizeof pcbuf, "-");
    }
    tui_printf(t, 1, 4, t->w - 2, TUI_PLAIN,
               "used %s  free %s  mapped %s  total %s  use %s  blocks %u", used,
               freed, mapped, total, pcbuf, v->blocks);
    if (p >= 0)
        tui_progress(t, t->w - 14, 4, 12, p);
    tui_table_draw(t, r, &a->storage, v->nsub, storage_cell, NULL, a);

    if (a->err[a->sub][0] != '\0')
        tui_printf(t, 1, t->h - 2, t->w - 2, TUI_BAD, "! %s", a->err[a->sub]);
    else if (v->unalloc_valid)
        tui_printf(t, 1, t->h - 2, t->w - 2, TUI_LABEL,
                   "unallocated %llu in %u blocks, largest %llu",
                   (unsigned long long)v->unalloc, v->unalloc_blocks,
                   (unsigned long long)v->largest_unalloc);
}

static void draw_system(struct tui *t, const struct app *a) {
    const struct zsys *z = &a->sys;

    if (!a->have_sys) {
        tui_printf(t, 1, 4, t->w - 2, TUI_BAD, "%s", a->sys_err);
        return;
    }
    tui_printf(t, 1, 4, t->w - 2, TUI_LABEL, "system     %s", z->sysname);
    tui_printf(t, 1, 5, t->w - 2, TUI_PLAIN, "host       %s", z->host);
    tui_printf(t, 1, 6, t->w - 2, TUI_PLAIN, "os         %s", z->os);
    tui_printf(t, 1, 7, t->w - 2, TUI_PLAIN, "machine    %s", z->machine);
    tui_printf(t, 1, 8, t->w - 2, TUI_PLAIN, "spaces     %d used, %u slots",
               z->njobs, z->slots);
    if (z->ipl_ok)
        tui_printf(t, 1, 9, t->w - 2, TUI_PLAIN,
                   "ipl        %04u-%02u-%02u %02u:%02u:%02u", z->ipl.year,
                   z->ipl.month, z->ipl.day, z->ipl.hour, z->ipl.minute,
                   z->ipl.second);
    tui_printf(t, 1, 10, t->w - 2, TUI_PLAIN, "real       %llu K online at IPL",
               (unsigned long long)z->real_k);
    tui_printf(
        t, 1, 12, t->w - 2, TUI_LABEL,
        "storage    CSA %llu unallocated, PVT %llu",
        (unsigned long long)(a->info[0].unalloc_valid ? a->info[0].unalloc : 0),
        (unsigned long long)(a->info[3].unalloc_valid ? a->info[3].unalloc
                                                      : 0));
}

static void draw_paging(struct tui *t, const struct app *a) {
    const struct zpage *p = &a->page;

    if (!a->have_page) {
        tui_printf(t, 1, 4, t->w - 2, TUI_BAD, "%s", a->page_err);
        return;
    }
    tui_printf(t, 1, 4, t->w - 2, TUI_LABEL, "aux storage  ASMVT %08X",
               (unsigned int)p->asmvt);
    tui_printf(t, 1, 6, t->w - 2, p->common_pds ? TUI_GOOD : TUI_WARN,
               "common data set  %-3s  %s", p->common_pds ? "yes" : "no",
               p->common_full ? "full" : "");
    tui_printf(t, 1, 7, t->w - 2, p->plpa_pds ? TUI_GOOD : TUI_WARN,
               "plpa data set    %-3s  %s", p->plpa_pds ? "yes" : "no",
               p->plpa_full ? "full" : "");
    tui_printf(t, 1, 8, t->w - 2, TUI_PLAIN, "local paging     %s",
               p->local_paging ? "on" : "off");
    tui_printf(t, 1, 9, t->w - 2,
               p->quick_start || p->warm_start ? TUI_WARN : TUI_PLAIN,
               "start            %s%s", p->quick_start ? "quick" : "cold",
               p->warm_start ? ", warm VIO" : "");
    tui_printf(t, 1, 10, t->w - 2, TUI_PLAIN,
               "vio journal      %s, %u control intervals",
               p->vio_journaling ? "active" : "off", p->vio_ci);
}

static void draw_jobs(struct tui *t, struct app *a) {
    tui_rect r = {0, 3, t->w, t->h - 5};
    char head[24];
    struct tui_col cols[3];

    if (a->have_sys) {
        for (int i = 0; i < a->sys.njobs; i++)
            a->job_order[i] = i;
        sort_sys = &a->sys;
        sort_key = a->job_sort;
        sort_dir = a->job_dir;
        qsort(a->job_order, (size_t)a->sys.njobs, sizeof a->job_order[0],
              cmp_job);
        (void)memcpy(cols, job_cols, sizeof cols);
        (void)snprintf(head, sizeof head, "%s %s", job_sort_names[a->job_sort],
                       a->job_dir < 0 ? "^" : "v");
        cols[a->job_sort].title = head;
        a->jobs.cols = cols;
    }
    if (!a->have_sys) {
        tui_printf(t, 1, 4, t->w - 2, TUI_BAD, "%s", a->sys_err);
        return;
    }
    tui_table_draw(t, r, &a->jobs, a->sys.njobs, job_cell, NULL, a);
    tui_printf(t, 1, t->h - 2, t->w - 2, TUI_LABEL,
               "%d address spaces, %u slots", a->sys.njobs, a->sys.slots);
}

/** @brief Everything about the selected subpool, over the table. */
static void draw_subpool(struct tui *t, const struct app *a) {
    const struct vsm_info *v = &a->info[a->sub];
    int i = v->nsub ? a->sub_order[a->storage.vp.sel] : 0;
    const struct vsm_subpool *s = &v->sub[i];
    int w = 46;
    int h = 12;
    int x = (t->w - w) / 2;
    int y = (t->h - h) / 2;
    char u[16], f[16], m[16], l[16];

    if (x < 0)
        x = 0;
    if (y < 0)
        y = 0;
    human(s->used, u, sizeof u);
    human(s->free, f, sizeof f);
    human(s->foot, m, sizeof m);
    human(s->largest, l, sizeof l);
    tui_fill(t, x, y, w, h, ' ', TUI_PLAIN);
    tui_box(t, x, y, w, h, TUI_LABEL);
    tui_printf(t, x + 2, y + 1, w - 4, TUI_TITLE, "subpool %u", s->id);
    tui_printf(t, x + 2, y + 3, w - 4, TUI_PLAIN, "key        %u", s->key);
    tui_printf(t, x + 2, y + 4, w - 4, TUI_PLAIN, "blocks     %u", s->blocks);
    tui_printf(t, x + 2, y + 5, w - 4, TUI_PLAIN, "used       %s", u);
    tui_printf(t, x + 2, y + 6, w - 4, TUI_PLAIN, "free       %s", f);
    tui_printf(t, x + 2, y + 7, w - 4, TUI_PLAIN, "mapped     %s", m);
    tui_printf(t, x + 2, y + 8, w - 4, TUI_PLAIN, "largest    %s", l);
    if (v->area == VSM_PVT)
        tui_printf(t, x + 2, y + 9, w - 4, TUI_PLAIN, "tcb        %08X",
                   (unsigned int)s->tcb);
    else
        tui_printf(t, x + 2, y + 9, w - 4, TUI_PLAIN, "address    %08X-%08X",
                   (unsigned int)s->lo, (unsigned int)s->hi);
    tui_printf(t, x + 2, y + h - 2, w - 4, TUI_STYLE(7, 0, TUI_DIM),
               "any key closes");
}

static void draw_help(struct tui *t) {
    int w = t->w < 64 ? t->w - 2 : 62;
    int h = 19;
    int x = (t->w - w) / 2;
    int y = (t->h - h) / 2;

    if (y < 0)
        y = 0;
    tui_fill(t, x, y, w, h, ' ', TUI_PLAIN);
    tui_box(t, x, y, w, h, TUI_LABEL);
    tui_printf(t, x + 2, y + 1, w - 4, TUI_TITLE, "ztop keys");

    tui_printf(t, x + 2, y + 3, w - 4, TUI_LABEL, "views");
    tui_printf(t, x + 2, y + 4, w - 4, TUI_PLAIN,
               "TAB / Shift-TAB   next / previous view");
    tui_printf(t, x + 2, y + 5, w - 4, TUI_PLAIN,
               "1 2 3 4           inside STORAGE: CSA SQA LSQA PVT");
    tui_printf(t, x + 2, y + 6, w - 4, TUI_PLAIN,
               "click a tab       same, with the mouse");

    tui_printf(t, x + 2, y + 8, w - 4, TUI_LABEL, "moving");
    tui_printf(t, x + 2, y + 9, w - 4, TUI_PLAIN, "j k / arrows      one row");
    tui_printf(t, x + 2, y + 10, w - 4, TUI_PLAIN, "PgUp PgDn         a page");
    tui_printf(t, x + 2, y + 11, w - 4, TUI_PLAIN,
               "g G / Home End    first / last");
    tui_printf(t, x + 2, y + 12, w - 4, TUI_PLAIN,
               "wheel, click      scroll, select a row");
    tui_printf(t, x + 2, y + 13, w - 4, TUI_PLAIN,
               "s / S             sort / reverse   Enter  details");

    tui_printf(t, x + 2, y + 14, w - 4, TUI_LABEL, "the screen");
    tui_printf(t, x + 2, y + 15, w - 4, TUI_PLAIN,
               "p pause   r refresh now   + - interval 1..60 s");
    tui_printf(t, x + 2, y + 16, w - 4, TUI_PLAIN,
               "? close this help         q quit");
}

/** @brief The right half of the status line: what is under the cursor. */
static void detail(struct app *a, char *buf, size_t len) {
    if (a->paused) {
        (void)snprintf(buf, len, "PAUSED");
    } else if (a->tab == 0 && a->have[a->sub] && a->info[a->sub].nsub > 0) {
        const struct vsm_subpool *sp = &a->info[a->sub].sub[a->storage.vp.sel];

        if (a->sub == 3)
            (void)snprintf(buf, len, "sp %u  tcb %08X", sp->id,
                           (unsigned int)sp->tcb);
        else if (sp->blocks == 0)
            (void)snprintf(buf, len, "sp %u  empty", sp->id);
        else
            (void)snprintf(buf, len, "sp %u  %08X-%08X", sp->id,
                           (unsigned int)sp->lo, (unsigned int)sp->hi);
    } else if (a->tab == 2 && a->have_sys && a->sys.njobs > 0) {
        const struct zjob *j = &a->sys.job[a->jobs.vp.sel];

        (void)snprintf(buf, len, "asid %u  %s", j->asid, j->name);
    } else {
        buf[0] = '\0';
    }
}

static void view(void *model, struct tui *t) {
    struct app *a = model;
    char right[48];

    tui_clear(t);
    draw_header(t, a);
    draw_tabs(t, a);
    tui_fill(t, 0, 2, t->w, 1, '-', TUI_STYLE(7, 0, TUI_DIM));

    if (t->w < 70 || t->h < 14) {
        tui_printf(t, 1, 3, 0, TUI_BAD, "terminal too small: need 70x14");
    } else if (a->tab == 0) {
        draw_storage(t, a);
    } else if (a->tab == 1) {
        draw_system(t, a);
    } else if (a->tab == 2) {
        draw_jobs(t, a);
    } else {
        draw_paging(t, a);
    }
    if (a->popup)
        draw_subpool(t, a);
    if (a->help)
        draw_help(t);
    detail(a, right, sizeof right);
    tui_statusbar(t,
                  a->tab == 0   ? "TAB view  1-4 area  j/k move  s sort  ? help"
                  : a->tab == 1 ? "TAB view  r refresh  ? help"
                  : a->tab == 2 ? "TAB view  j/k move  s sort  ? help"
                                : "TAB view  r refresh  ? help",
                  right);
}

static bool parse_areas(const char *s, unsigned int *mask) {
    unsigned int m = 0;

    while (*s != '\0') {
        size_t n = strcspn(s, ",");

        if (n == 3 && strncmp(s, "csa", 3) == 0)
            m |= 1u << 0;
        else if (n == 3 && strncmp(s, "sqa", 3) == 0)
            m |= 1u << 1;
        else if (n == 4 && strncmp(s, "lsqa", 4) == 0)
            m |= 1u << 2;
        else if (n == 3 && strncmp(s, "pvt", 3) == 0)
            m |= 1u << 3;
        else if (n == 3 && strncmp(s, "all", 3) == 0)
            m |= 0xFu;
        else
            return false;
        s += n;
        if (*s == ',')
            s++;
    }
    *mask = m;
    return m != 0;
}

static void usage(FILE *f) {
    (void)fprintf(
        f,
        "usage: ztop [-a csa|sqa|lsqa|pvt|all] [-s] [-j] [-p] [-x] [-d "
        "[seconds]]\n"
        "  -a  storage areas to report (default csa); comma-separate or all\n"
        "  -s  system name, host, release, IPL and address-space count\n"
        "  -j  one line per address space\n"
        "  -p  auxiliary storage: page data sets, local paging, VIO journal\n"
        "  -x  dump the VSMLIST work and data areas (one-shot only)\n"
        "  -d  force the interactive view (the default on a terminal)\n"
        "  -h  this text\n");
}

static void wheel(struct app *a, int dir, int height) {
    if (a->tab == 0)
        tui_viewport_move(&a->storage.vp, dir, a->info[a->sub].nsub, height);
    else if (a->tab == 2)
        tui_viewport_move(&a->jobs.vp, dir, a->sys.njobs, height);
}

/** @brief Tabs, sub-tabs, table rows and the wheel. */
static bool mouse_event(struct tui_app *app, const struct tui_msg *m) {
    static const char *const views[3] = {"1 STORAGE", "2 SYSTEM", "3 JOBS"};
    struct app *a = app->model;
    const struct tui_mouse *mo = &m->mouse;
    int idx, row, height;

    height = (a->tab == 0 ? m->h - 8 : m->h - 6);
    if (height < 1)
        height = 1;
    if (mo->button == 64 || mo->button == 65) {
        wheel(a, mo->button == 64 ? -1 : 1, height);
        return true;
    }
    if (mo->release || mo->button != 0)
        return false;
    if (mo->y == 1) {
        idx = tui_tabs_hit(mo->x, views, 3);
        if (idx < 0)
            return false;
        a->tab = idx;
        return true;
    }
    if (a->tab == 0 && mo->y == 3) {
        if (mo->x >= 1 && mo->x <= 5)
            a->sub = 0;
        else if (mo->x >= 7 && mo->x <= 11)
            a->sub = 1;
        else if (mo->x >= 13 && mo->x <= 18)
            a->sub = 2;
        else if (mo->x >= 20 && mo->x <= 24)
            a->sub = 3;
        else
            return false;
        return true;
    }
    if (a->tab == 0 && mo->y >= 6) {
        row = a->storage.vp.top + (mo->y - 6);
        if (row < a->info[a->sub].nsub) {
            a->storage.vp.sel = row;
            return true;
        }
    } else if (a->tab == 2 && mo->y >= 4) {
        row = a->jobs.vp.top + (mo->y - 4);
        if (row < a->sys.njobs) {
            a->jobs.vp.sel = row;
            return true;
        }
    }
    return false;
}

static bool update(struct tui_app *app, const struct tui_msg *m) {
    struct app *a = app->model;
    int rows, height;

    if (a->help) {
        if (m->type == TUI_MSG_KEY && (m->key == '?' || m->key == TUI_KEY_ESC ||
                                       m->key == 'q' || m->key == 0x03))
            a->help = false;
        return true;
    }
    if (a->popup) {
        if (m->type == TUI_MSG_KEY)
            a->popup = false;
        return true;
    }
    if (m->type == TUI_MSG_TICK) {
        if (!a->paused)
            collect(a);
        return true;
    }
    if (m->type == TUI_MSG_RESIZE)
        return true;
    if (m->type == TUI_MSG_MOUSE)
        return mouse_event(app, m);
    if (m->type != TUI_MSG_KEY)
        return false;

    if (m->key == 'q' || m->key == TUI_KEY_ESC || m->key == 0x03) {
        app->quit = true;
        return false;
    }
    if (m->key == '?') {
        a->help = true;
        return true;
    }
    if (m->key == 'p') {
        a->paused = !a->paused;
        return true;
    }
    if (m->key == 'r') {
        collect(a);
        return true;
    }
    if (m->key == '+' || m->key == '=') {
        if (a->interval < 60)
            a->interval++;
        a->tapp->tick_ms = a->interval * 1000;
        return true;
    }
    if (m->key == '-' && a->interval > 1) {
        a->interval--;
        a->tapp->tick_ms = a->interval * 1000;
        return true;
    }
    if (m->key == TUI_KEY_TAB) {
        a->tab = (a->tab + 1) % 4;
        return true;
    }
    if (m->key == TUI_KEY_BACKTAB) {
        a->tab = (a->tab + 3) % 4;
        return true;
    }
    if (a->tab == 0 && m->key >= '1' && m->key <= '4') {
        a->sub = m->key - '1';
        a->storage.vp.sel = 0;
        a->storage.vp.top = 0;
        return true;
    }
    if ((m->key == 's' || m->key == 'S') && a->tab != 1) {
        int *key = a->tab == 0 ? &a->sub_sort : &a->job_sort;
        int *dir = a->tab == 0 ? &a->sub_dir : &a->job_dir;
        int n = a->tab == 0 ? SS_COUNT : JS_COUNT;

        if (m->key == 's')
            *key = (*key + 1) % n;
        else
            *dir = -*dir;
        return true;
    }
    if (a->tab == 0 && (m->key == '\r' || m->key == '\n') &&
        a->info[a->sub].nsub > 0) {
        a->popup = true;
        return true;
    }

    rows = a->tab == 0 ? a->info[a->sub].nsub : a->sys.njobs;
    height = a->tab == 0 ? m->h - 8 : m->h - 6;
    if (height < 1)
        height = 1;
    if (a->tab == 0)
        return tui_viewport_key(&a->storage.vp, m->key, rows, height);
    if (a->tab == 2)
        return tui_viewport_key(&a->jobs.vp, m->key, rows, height);
    return false;
}

static void sys_fill(struct zsys *z, const char *host) {
    struct utsname u;

    if (uname(&u) == 0) {
        (void)snprintf(z->os, sizeof z->os, "%s %s", u.sysname, u.release);
        (void)snprintf(z->machine, sizeof z->machine, "%s", u.machine);
    }
    (void)snprintf(z->host, sizeof z->host, "%s", host);
}

static void run_tui(int interval) {
    static struct app a;
    struct tui t;
    struct tui_app ta;

    memset(&a, 0, sizeof a);
    if (gethostname(a.host, sizeof a.host - 1) != 0)
        (void)snprintf(a.host, sizeof a.host, "?");
    a.interval = interval;
    a.sub_sort = SS_USED;
    a.sub_dir = -1;
    a.job_sort = JS_ASID;
    a.job_dir = 1;
    a.storage.cols = storage_cols;
    a.storage.ncols = 8;
    a.jobs.cols = job_cols;
    a.jobs.ncols = 3;

    if (!tui_open(&t)) {
        (void)fprintf(stderr, "ztop: -d needs a terminal\n");
        exit(1);
    }
    collect(&a);

    memset(&ta, 0, sizeof ta);
    ta.model = &a;
    ta.update = update;
    ta.view = view;
    ta.tick_ms = interval * 1000;
    a.tapp = &ta;
    tui_run(&t, &ta);
    tui_close(&t);
}

static void run_oneshot(unsigned mask, bool want_sys, bool want_jobs,
                        bool want_page, bool dump) {
    char host[65];
    bool printed = false;
    int rc = 0;
    int i;

    for (i = 0; i < 4; i++) {
        struct vsm_info v;

        if ((mask & (1u << i)) == 0)
            continue;
        if (!collect_area(&v, i + 1, dump)) {
            (void)fprintf(stderr, "ztop: %s: %s\n", area_name[i], v.err);
            rc = 1;
            continue;
        }
        if (printed)
            (void)printf("\n");
        print_area(&v);
        printed = true;
    }
    if (want_page) {
        struct zpage p;

        if (!zpage_read(&p)) {
            (void)fprintf(stderr, "ztop: paging is only available on z/OS\n");
            rc = 1;
        } else {
            if (printed)
                (void)printf("\n");
            print_page(&p);
            printed = true;
        }
    }
    if (want_sys || want_jobs) {
        struct zsys z;

        if (gethostname(host, sizeof host - 1) != 0)
            (void)snprintf(host, sizeof host, "?");
        host[sizeof host - 1] = '\0';
        if (!zsys_read(&z)) {
            (void)fprintf(stderr,
                          "ztop: system and jobs are only available on z/OS\n");
            rc = 1;
        } else {
            sys_fill(&z, host);
            if (printed)
                (void)printf("\n");
            if (want_sys)
                print_system(&z);
            if (want_jobs) {
                if (want_sys)
                    (void)printf("\n");
                print_jobs(&z);
            }
        }
    }
    exit(rc);
}

int main(int argc, char **argv) {
    unsigned mask = 0;
    bool dump = false;
    bool interactive = false;
    bool want_sys = false;
    bool want_jobs = false;
    bool want_page = false;
    int interval = 0;
    int i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0) {
            usage(stdout);
            return 0;
        } else if (strcmp(argv[i], "-x") == 0) {
            dump = true;
        } else if (strcmp(argv[i], "-s") == 0) {
            want_sys = true;
        } else if (strcmp(argv[i], "-j") == 0) {
            want_jobs = true;
        } else if (strcmp(argv[i], "-p") == 0) {
            want_page = true;
        } else if (strcmp(argv[i], "-a") == 0) {
            if (i + 1 >= argc || !parse_areas(argv[++i], &mask)) {
                usage(stderr);
                return 2;
            }
        } else if (strncmp(argv[i], "-a", 2) == 0 && argv[i][2] != '\0') {
            if (!parse_areas(argv[i] + 2, &mask)) {
                usage(stderr);
                return 2;
            }
        } else if (strcmp(argv[i], "-d") == 0) {
            interactive = true;
            if (i + 1 < argc && argv[i + 1][0] >= '0' &&
                argv[i + 1][0] <= '9') {
                long secs = strtol(argv[++i], NULL, 10);

                interval = secs < 1 ? 1 : secs > 60 ? 60 : (int)secs;
            }
        } else {
            usage(stderr);
            return 2;
        }
    }

    /* A bare `ztop` on a terminal is the full-screen view; anything that
     * selects a report, or a pipe, keeps the one-shot output. */
    if (!interactive && !dump && !want_sys && !want_jobs && !want_page &&
        mask == 0 && isatty(STDOUT_FILENO))
        interactive = true;

    if (interactive) {
        if (dump)
            (void)fprintf(stderr, "ztop: -x is ignored with -d\n");
        if (interval < 1)
            interval = 10;
        if (interval > 60)
            interval = 60;
        run_tui(interval);
    } else {
        if (mask == 0 && !want_sys && !want_jobs && !want_page)
            mask = 1u;
        run_oneshot(mask, want_sys, want_jobs, want_page, dump);
    }
    return 0;
}
