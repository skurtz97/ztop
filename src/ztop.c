/**
 * @file ztop.c
 * @brief What z/OS has where: storage by subpool, the system, and jobs.
 *
 * One-shot reports for scripts and a full-screen view for people.  The
 * view is built on libtui, which knows nothing about z/OS: ztop supplies
 * the model, the messages and the tables.
 */
#include <ctype.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

#include "sys.h"
#include "mdb.h"
#include "uss.h"
#include "tui.h"
#include "vsm.h"
#include "zmcs.h"

static const char ZTOP_VERSION[] = "0.3";

/* The console is opt-in: opening it calls MODESET, and an unauthorized
 * program does not get a return code from that, it gets S047. */
static bool console_opt;

/* The stub in src/zmcs.s addresses these fields by offset, and those are
 * the -m32 offsets: a pointer is 4 bytes there, so the same struct on a
 * 64-bit host has a different shape and the check would only be noise. */
#if defined(__MVS__)
static_assert(offsetof(zmcs_req, save) == 40, "zmcs_req.save");
static_assert(offsetof(zmcs_req, csa) == 184, "zmcs_req.csa");
static_assert(offsetof(zmcs_req, alet) == 32, "zmcs_req.alet");
static_assert(offsetof(zmcs_req, ecb) == 28, "zmcs_req.ecb");
static_assert(offsetof(zmcs_req, rsn) == 24, "zmcs_req.rsn");
static_assert(sizeof(zmcs_req) == 188, "zmcs_req: runs into the next object");
#endif

#if defined(__MVS__)
/* HLASM upcases the ESD name of the CSECT, so the assembler side is
 * exported as ZVSLIST whatever case the source is written in. */
extern int zvslist(void *) __asm__("ZVSLIST");
#pragma linkage(zvslist, OS)

/* The 31-bit request block.  R13 is saved by the stub, so the save area
 * has to live in storage the C compiler gives us, not in the read-only
 * CSECT. */
typedef struct zreq {
    int area;
    unsigned len;
    void *wk;
    int rc;
    void *self;
    unsigned long long save[18];
    int space;
} zreq;
static_assert(offsetof(zreq, self) == 16, "zreq.self");
static_assert(offsetof(zreq, save) == 24, "zreq.save");
static_assert(offsetof(zreq, space) == 168, "zreq.space");

static constexpr unsigned WORK_SIZE = 1024 * 1024;
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

static void dump_work(const vsm_info *v, const unsigned char *workp,
                      const unsigned char *data, unsigned int len, int rc) {
    (void)fprintf(stderr, "ztop: VSMLIST %s %s rc=%d data=%08X len=%u\n",
                  area_name[v->area - 1],
                  v->space == VSM_UNALLOC ? "UNALLOC" : "FREE", rc,
                  (unsigned int)(uintptr_t)data, len);
    hexdump("work", workp, 32, 32);
    if (data != nullptr && len > 0)
        hexdump("data", data, len, 256);
}

static bool fetch_one(vsm_info *v, int area, int space, bool dump) {
    int calls = 0;
    const unsigned char *data;
    unsigned int len;

    vsm_reset(v, (unsigned int)area, (unsigned int)space);
    memset(work, 0, 4); /* the return code must start at 0 */
    for (;;) {
        zreq req = {0};
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
static bool fetch_one(vsm_info *v, int area, int space, bool dump) {
    (void)area;
    (void)space;
    (void)dump;
    (void)snprintf(v->err, sizeof v->err, "VSMLIST is only available on z/OS");
    return false;
}
#endif

static bool collect_area(vsm_info *v, int area, bool dump) {
    if (!fetch_one(v, area, VSM_FREE, dump))
        return false;
    if (area == VSM_CSA || area == VSM_PVT) {
        vsm_info u;

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
/* Only CSA and PVT get an unallocated pass, so only they have a total. */
static bool has_total(const vsm_info *v) {
    return (v->area == VSM_CSA || v->area == VSM_PVT) && v->unalloc_valid;
}

static int pct(const vsm_info *v) {
    uint64_t total;

    if (!has_total(v))
        return -1;
    total = v->foot + v->unalloc;
    if (total == 0)
        return 0;
    return (int)((v->used * 100u + total / 2) / total);
}

static void print_area(const vsm_info *v) {
    const char *name = area_name[v->area - 1];
    bool pvt = v->area == VSM_PVT;
    int p = pct(v);
    int i;

    (void)printf("area %s: used %llu (below 16M %llu above 16M %llu) free %llu "
                 "mapped %llu blocks %u",
                 name, (unsigned long long)v->used,
                 (unsigned long long)v->below, (unsigned long long)v->above,
                 (unsigned long long)v->free, (unsigned long long)v->foot,
                 v->blocks);
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
        const vsm_subpool *s = &v->sub[i];
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

static void print_system(const zsys *z) {
    (void)printf(
        "system %s  host %s  os %s  machine %s  address spaces %d/%u\n",
        z->sysname, z->host, z->os, z->machine, z->njobs, z->slots);
    if (z->ipl_ok)
        (void)printf("ipl %04u-%02u-%02u %02u:%02u:%02u  real storage %llu K\n",
                     z->ipl.year, z->ipl.month, z->ipl.day, z->ipl.hour,
                     z->ipl.minute, z->ipl.second,
                     (unsigned long long)z->real_k);
}

static void print_page(const zpage *p) {
    (void)printf("aux storage  common %s%s  plpa %s%s  local %s  vio %s"
                 ", journal %u\n",
                 p->common_pds ? "yes" : "no", p->common_full ? " full" : "",
                 p->plpa_pds ? "yes" : "no", p->plpa_full ? " full" : "",
                 p->local_paging ? "on" : "off",
                 p->vio_journaling ? "on" : "off", p->vio_ci);
}

static void print_jobs(const zsys *z) {
    int i;

    (void)printf("%4s %-8s %8s\n", "asid", "job", "ascb");
    for (i = 0; i < z->njobs; i++)
        (void)printf("%4u %-8s %08X\n", z->job[i].asid, z->job[i].name,
                     (unsigned int)z->job[i].ascb);
}

typedef struct pvt_row {
    uint32_t tcb;
    unsigned nsub;
    uint64_t used;
    uint64_t below;
    uint64_t above;
} pvt_row;

/* The console ring: 256 lines of 200 columns is 51K inside the model,
 * and it never grows. */
enum { LOG_LINES = 256, LOG_COLS = 200 };

typedef struct app {
    vsm_info info[4];
    bool have[4];
    char err[4][160];
    zsys sys;
    bool have_sys;
    char sys_err[160];
    zpage page;
    bool have_page;
    char page_err[160];
    int interval;
    int collect_ms;
    bool paused;
    bool help;
    int tab;
    int sub;
    tui_table storage;
    tui_table jobs;
    tui_app *tapp;
    char host[65];
    int sub_sort, sub_dir;
    int job_sort, job_dir;
    int sub_order[VSM_MAX_SUB];
    int job_order[ZJOB_MAX];
    bool popup;
    bool pvt_fold;
    bool tcb_popup;
    bool inspector;
    char filter[32];
    tui_input fin;
    tui_input cin;
    bool filtering;
    bool commanding;
    int sub_rows;
    int job_rows;
    unsigned char trend[4][32]; /* per-area use%, newest last */
    int ntrend[4];
    tui_viewport pvt_vp;
    pvt_row pvt[VSM_MAX_SUB];
    int npvt;
    uss_ps_list ps;
    uss_fs_list fs;
    bool have_ps;
    bool have_fs;
    tui_table pst;
    tui_table fst;
    /* The console log: one request block, one ring of lines, both in the
     * model, because a refresh must not allocate. */
    zmcs_req creq;
    char creq_name[8];
    unsigned creq_consid;
    char mdb[ZMCS_MDB_MAX];
    int mdb_len;
    char log[LOG_LINES][LOG_COLS];
    int nlog;
    int log_next;
    bool log_on;
    char log_err[160];
    tui_viewport log_vp;
} app;

enum { SS_SP, SS_KEY, SS_BLOCKS, SS_USED, SS_FREE, SS_LARGEST, SS_COUNT };
enum { JS_ASID, JS_JOB, JS_ADDR, JS_COUNT };

static const char *const sub_sort_names[SS_COUNT] = {
    "sp", "key", "blocks", "used", "free", "largest",
};
static const char *const job_sort_names[JS_COUNT] = {"asid", "job", "ascb"};

/* qsort has no context argument; these are set right before each call and
 * the application is single-threaded. */
static const vsm_info *sort_info;
static const zsys *sort_sys;
static int sort_key, sort_dir;

static int cmp_u64(uint64_t a, uint64_t b) {
    return (a > b) - (a < b);
}
/* ---- the folded PVT view: one row per TCB ---- */

/* Defined with the filter, needed by the folded table and by collect. */
static bool match(const char *hay, const char *pat);
static void order_storage(app *a);

/* Defined below; the tick and the command bar use them. */
static void log_start(app *a);
static void log_poll(app *a);
static void log_stop(app *a);

/* The console is named after the caller, so two people (or two sessions)
 * do not collide on one name and the RACF profile can be that user's
 * alone.  The service checks MVS.MCSOPER.<name>, so the name is not a
 * secret: an unauthorised user choosing someone else's name is refused. */
static void console_name(char *name) {
    const char *from = getenv("LOGNAME");

    if (from == nullptr || *from == '\0')
        from = getenv("USER");
    (void)memset(name, ' ', 8);
    if (from == nullptr)
        from = "ZTOP";
    for (int i = 0; i < 8 && from[i] != '\0'; i++) {
        unsigned char c = (unsigned char)from[i];

        if (c >= 'a' && c <= 'z')
            c = (unsigned char)(c - 'a' + 'A');
        name[i] =
            (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ? (char)c : '_';
    }
}

static int tcb_order[VSM_MAX_SUB];

static int cmp_tcb(const void *pa, const void *pb) {
    const vsm_subpool *a = &sort_info->sub[*(const int *)pa];
    const vsm_subpool *b = &sort_info->sub[*(const int *)pb];

    return (a->tcb > b->tcb) - (a->tcb < b->tcb);
}

/* Group the subpools by owning TCB.  Sorted first, so the grouping is a
 * linear pass and the row table needs no allocation. */
static void build_pvt(app *a) {
    const vsm_info *v = &a->info[3];

    a->npvt = 0;
    if (!a->have[3])
        return;
    for (int i = 0; i < v->nsub; i++)
        tcb_order[i] = i;
    sort_info = v;
    qsort(tcb_order, (size_t)v->nsub, sizeof tcb_order[0], cmp_tcb);
    for (int i = 0; i < v->nsub; i++) {
        const vsm_subpool *s = &v->sub[tcb_order[i]];
        pvt_row *r;

        if (a->npvt > 0 && a->pvt[a->npvt - 1].tcb == s->tcb) {
            r = &a->pvt[a->npvt - 1];
        } else {
            if (a->npvt >= VSM_MAX_SUB)
                break;
            r = &a->pvt[a->npvt++];
            *r = (pvt_row){s->tcb, 0, 0, 0, 0};
        }
        r->nsub++;
        r->used += s->used;
        r->below += s->below;
        r->above += s->above;
    }
    {
        int n = 0;

        for (int i = 0; i < a->npvt; i++) {
            char key[16];

            (void)snprintf(key, sizeof key, "%08X",
                           (unsigned int)a->pvt[i].tcb);
            if (match(key, a->filter))
                a->pvt[n++] = a->pvt[i];
        }
        a->npvt = n;
    }
}

/* The viewport and row count the keys act on, so a folded PVT and the JOBS
 * table never need their own copy of the movement code. */
static bool folded(const app *a) {
    return a->tab == 0 && a->sub == 3 && a->pvt_fold;
}

static tui_viewport *vp_of(app *a) {
    if (folded(a))
        return &a->pvt_vp;
    if (a->tab == 2)
        return &a->jobs.vp;
    if (a->tab == 4)
        return &a->pst.vp;
    if (a->tab == 5)
        return &a->fst.vp;
    if (a->tab == 6)
        return &a->log_vp;
    return &a->storage.vp;
}

static int vrows(const app *a) {
    if (folded(a))
        return a->npvt;
    if (a->tab == 2)
        return a->job_rows;
    if (a->tab == 4)
        return a->ps.nproc;
    if (a->tab == 5)
        return a->fs.nfs;
    if (a->tab == 6)
        return a->nlog;
    return a->sub_rows;
}

static int vheight(const app *a, int h) {
    if (a->tab == 0)
        return h - 8;
    if (a->tab == 2 || a->tab == 4 || a->tab == 5 || a->tab == 6)
        return h - 6;
    return 0;
}

static int cmp_u32(uint32_t a, uint32_t b) {
    return (a > b) - (a < b);
}

static int cmp_sub(const void *pa, const void *pb) {
    int ia = *(const int *)pa;
    int ib = *(const int *)pb;
    const vsm_subpool *a = &sort_info->sub[ia];
    const vsm_subpool *b = &sort_info->sub[ib];
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
    const zjob *a = &sort_sys->job[ia];
    const zjob *b = &sort_sys->job[ib];
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

static const tui_col storage_cols[] = {
    {"SP", 3, true},       {"KY", 3, true},        {"BLKS", 8, true},
    {"USED", 10, true},    {"FREE", 10, true},     {"MAPPED", 10, true},
    {"LARGEST", 10, true}, {"ADDRESS", 17, false},
};

static const tui_col pvt_cols[] = {
    {"SP", 3, true},       {"KY", 3, true},    {"BLKS", 7, true},
    {"USED", 10, true},    {"FREE", 10, true}, {"MAPPED", 10, true},
    {"LARGEST", 10, true}, {"TCB", 9, false},
};

/* The folded PVT table: one row per TCB, with the 16M split and which side
 * of the line the TCB's storage is on. */
static const tui_col pvt_fold_cols[] = {
    {"TCB", 9, false},  {"SUBS", 5, true},  {"USED", 10, true},
    {"<16M", 10, true}, {">16M", 10, true}, {"LINE", 5, false},
};

static const tui_col job_cols[] = {
    {"ASID", 6, true},
    {"JOB", 9, false},
    {"ASCB", 10, false},
};

static void pvt_cell(void *ctx, int row, int col, char *buf, size_t len) {
    const app *a = ctx;
    const pvt_row *r = &a->pvt[row];
    char u[16], b[16], ab[16];

    switch (col) {
    case 0:
        (void)snprintf(buf, len, "%08X", (unsigned int)r->tcb);
        break;
    case 1:
        (void)snprintf(buf, len, "%u", r->nsub);
        break;
    case 2:
        human(r->used, u, sizeof u);
        (void)snprintf(buf, len, "%s", u);
        break;
    case 3:
        human(r->below, b, sizeof b);
        (void)snprintf(buf, len, "%s", b);
        break;
    case 4:
        human(r->above, ab, sizeof ab);
        (void)snprintf(buf, len, "%s", ab);
        break;
    default:
        (void)snprintf(buf, len, "%s",
                       r->below != 0 && r->above != 0 ? "BOTH"
                       : r->above != 0                ? ">16M"
                                                      : "<16M");
        break;
    }
}

static void storage_cell(void *ctx, int row, int col, char *buf, size_t len) {
    const app *a = ctx;
    int i = row < a->sub_rows ? a->sub_order[row] : row;
    const vsm_subpool *s = &a->info[a->sub].sub[i];
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
    const app *a = ctx;
    int i = row < a->job_rows ? a->job_order[row] : row;
    const zjob *j = &a->sys.job[i];

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

static void sys_read(app *a) {
    zsys z;
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

/* Keep the last 32 samples of an area's use%, so the storage view can draw
 * its recent shape without keeping history anywhere else. */
static void push_trend(app *a, int i, int p) {
    unsigned char *t = a->trend[i];
    int *n = &a->ntrend[i];

    if (p < 0 || p > 100)
        return;
    if (*n == (int)sizeof a->trend[i]) {
        (void)memmove(t, t + 1, sizeof a->trend[i] - 1);
        (*n)--;
    }
    t[(*n)++] = (unsigned char)p;
}

static void collect(app *a) {
    struct timespec t0, t1;
    int i;

    (void)clock_gettime(CLOCK_MONOTONIC, &t0);
    for (i = 0; i < 4; i++) {
        vsm_info v;

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
    build_pvt(a);
    order_storage(a);
    if (a->tab == 4)
        a->have_ps = uss_ps_read(&a->ps);
    else if (a->tab == 5)
        a->have_fs = uss_fs_read(&a->fs);
    log_poll(a);
    for (i = 0; i < 4; i++)
        if (a->have[i])
            push_trend(a, i, pct(&a->info[i]));
    (void)clock_gettime(CLOCK_MONOTONIC, &t1);
    a->collect_ms = (int)((t1.tv_sec - t0.tv_sec) * 1000 +
                          (t1.tv_nsec - t0.tv_nsec) / 1000000);
}

/* Tab bit for the KeyMap's per-view mask. */
enum {
    TABBITS_STORAGE = 1u << 0,
    TABBITS_SYSTEM = 1u << 1,
    TABBITS_JOBS = 1u << 2,
    TABBITS_PAGING = 1u << 3,
    TABBITS_PS = 1u << 4,
    TABBITS_FS = 1u << 5,
    TABBITS_LOG = 1u << 6,
    TABBITS_ALL = 0x7Fu,
    TABBITS_ROWS = TABBITS_STORAGE | TABBITS_JOBS | TABBITS_PS | TABBITS_FS |
        TABBITS_LOG,
};

/* One table drives the footer hint and the ? overlay, so a key that moves
 * cannot be documented in one place and forgotten in the other. */
static const tui_binding keymap[] = {
    /* views */
    {"TAB", "next view", "views", TABBITS_ALL, true},
    {"S-TAB", "previous view", "views", TABBITS_ALL, false},
    {"1-7", "area, or view", "views", TABBITS_ALL, false},
    {"click", "a tab", "views", TABBITS_ALL, false},
    {"/", "filter", "views", TABBITS_STORAGE | TABBITS_JOBS, false},
    {":", "command", "views", TABBITS_ALL, true},
    /* moving */
    {"j k", "one row", "moving", TABBITS_ROWS, false},
    {"PgUp PgDn", "a page", "moving", TABBITS_ROWS, false},
    {"g G", "first, last", "moving", TABBITS_ROWS, false},
    {"wheel", "scroll", "moving", TABBITS_ROWS, false},
    {"Enter", "details", "moving", TABBITS_STORAGE, false},
    {"i", "inspector", "moving", TABBITS_STORAGE, false},
    {"t", "fold TCBs", "moving", TABBITS_STORAGE, false},
    {"s S", "sort, reverse", "moving", TABBITS_STORAGE | TABBITS_JOBS, true},
    /* the screen */
    {"q", "quit", "the screen", TABBITS_ALL, true},
    {"p", "pause", "the screen", TABBITS_ALL, false},
    {"r", "refresh now", "the screen", TABBITS_ALL, false},
    {"+ -", "1..60 s", "the screen", TABBITS_ALL, false},
    {"?", "help", "the screen", TABBITS_ALL, true},
};
enum { KEYMAP_COUNT = (int)(sizeof keymap / sizeof keymap[0]) };

/* Drawn and hit-tested from the same array, so a click lands on the tab it
 * names. */
static const char *const tab_labels[7] = {
    "1 STORAGE", "2 SYSTEM", "3 JOBS", "4 PAGING", "5 PS", "6 FS", "7 LOG",
};

static void draw_header(tui *t, const app *a) {
    char now[16];
    char right[64];
    char ident[96];
    time_t clock_now = time(nullptr);
    struct tm tm;

    (void)localtime_r(&clock_now, &tm);
    (void)strftime(now, sizeof now, "%H:%M:%S", &tm);
    tui_fill(t, 0, 0, t->w, 1, ' ', TUI_BAR);
    tui_printf(t, 1, 0, 7, TUI_KEYCAP, " ztop ");
    tui_printf(t, 7, 0, 8, TUI_META, "%s |", ZTOP_VERSION);
    /* The identity stays on screen in every view: who you are looking at
     * should not need a tab. */
    if (a->have_sys)
        (void)snprintf(ident, sizeof ident, "%s | %s | %s", a->sys.sysname,
                       a->sys.os, a->sys.machine);
    else
        (void)snprintf(ident, sizeof ident, "%s", a->host);
    tui_printf(t, 14, 0, t->w - 40, TUI_PLAIN, "%s", ident);
    if (a->collect_ms > 0)
        (void)snprintf(right, sizeof right, "collect %dms   %s", a->collect_ms,
                       now);
    else
        (void)snprintf(right, sizeof right, "collect <1ms   %s", now);
    tui_printf(t, t->w - 1 - (int)strlen(right), 0, (int)strlen(right),
               TUI_PLAIN, "%s", right);
    if (a->paused)
        tui_printf(t, t->w - 9 - (int)strlen(right), 0, 8, TUI_WARN,
                   " PAUSED ");
}

static void draw_tabs(tui *t, const app *a) {
    int x = tui_tabs_draw(t, 0, 1, tab_labels, 7, a->tab);

    tui_printf(t, x + 1, 1, t->w - x - 2, TUI_META, "refresh %ds  +/-",
               a->interval);
}

/** @brief A line of text set into the panel's bottom border, like its
 * title. */
static void footer_line(tui *t, const char *text, tui_style st) {
    tui_printf(t, 3, t->h - 2, t->w - 6, st, " %s ", text);
}

/** @brief A dim label and its value, one blank apart. */
static void kv(tui *t, int x, int y, int maxw, const char *label,
               const char *value, tui_style st) {
    int ll = (int)strlen(label);

    if (ll >= maxw - 1)
        return;
    tui_printf(t, x, y, ll, TUI_META, "%s", label);
    tui_printf(t, x + ll + 1, y, maxw - ll - 1, st, "%s", value);
}

/** @brief kv on one row, abandoned when it would cross @p limit; returns the
 * column after it. */
static int pair(tui *t, int x, int y, const char *label, const char *value,
                tui_style st, int limit) {
    int n = (int)strlen(label) + 1 + (int)strlen(value);

    if (x + n > limit)
        return x;
    kv(t, x, y, n, label, value, st);
    return x + n;
}

/* Case-folded substring match, so /csa finds CSAPOOL. */
static bool match(const char *hay, const char *pat) {
    size_t n = strlen(pat);

    if (n == 0)
        return true;
    for (; *hay != '\0'; hay++) {
        size_t i = 0;

        while (i < n && hay[i] != '\0' &&
               tolower((unsigned char)hay[i]) == tolower((unsigned char)pat[i]))
            i++;
        if (i == n)
            return true;
    }
    return false;
}

/* The filter narrows the order array, so every reader of sub_order sees the
 * filtered list and the sort applies to what is left. */
static void order_storage(app *a) {
    const vsm_info *v = &a->info[a->sub];
    int n = 0;

    for (int i = 0; i < v->nsub; i++) {
        if (a->filter[0] != '\0') {
            char key[24];

            if (a->sub == 3)
                (void)snprintf(key, sizeof key, "%u %08X", v->sub[i].id,
                               (unsigned int)v->sub[i].tcb);
            else
                (void)snprintf(key, sizeof key, "%u", v->sub[i].id);
            if (!match(key, a->filter))
                continue;
        }
        a->sub_order[n++] = i;
    }
    a->sub_rows = n;
    sort_info = v;
    sort_key = a->sub_sort;
    sort_dir = a->sub_dir;
    qsort(a->sub_order, (size_t)n, sizeof a->sub_order[0], cmp_sub);
}

/* Bodies live with the other detail drawing; the storage view places them
 * either inside a popup or in the inspector pane. */
static void subpool_box(tui *t, const app *a, tui_rect r, int idx);
static void tcb_box(tui *t, const app *a, tui_rect r, int row);

/* One place for the row layout: the views draw from this rect and the
 * mouse hit-tests against the same one, so a header change cannot leave a
 * click target behind.  STORAGE keeps its area tabs on the row above. */
static tui_rect table_rect(int w, int h, int tab) {
    int y = tab == 0 ? 5 : 3;

    return (tui_rect){1, y, w - 2, h - y - 2};
}

static int table_rows(int w, int h, int tab) {
    int n = table_rect(w, h, tab).h - 1;

    return n > 0 ? n : 1;
}

static void draw_storage(tui *t, app *a) {
    const vsm_info *v = &a->info[a->sub];
    tui_rect r = table_rect(t->w, t->h, 0);
    tui_rect pane = {0, 0, 0, 0};
    bool side = a->inspector && t->w >= 110;
    char used[16], freed[16], mapped[16], total[16], pcbuf[8];
    char head[24];
    tui_col cols[8];
    static const int col_of[SS_COUNT] = {0, 1, 2, 3, 4, 6};
    int p = pct(v);

    order_storage(a);
    (void)memcpy(cols, a->sub == 3 ? pvt_cols : storage_cols, sizeof cols);
    (void)snprintf(head, sizeof head, "%s %s", sub_sort_names[a->sub_sort],
                   a->sub_dir < 0 ? "^" : "v");
    cols[col_of[a->sub_sort]].title = head;
    a->storage.cols = cols;
    tui_printf(t, 1, r.y - 2, 6, a->sub == 0 ? TUI_TAB_ON : TUI_TAB_OFF,
               " CSA ");
    tui_printf(t, 7, r.y - 2, 6, a->sub == 1 ? TUI_TAB_ON : TUI_TAB_OFF,
               " SQA ");
    tui_printf(t, 13, r.y - 2, 7, a->sub == 2 ? TUI_TAB_ON : TUI_TAB_OFF,
               " LSQA ");
    tui_printf(t, 20, r.y - 2, 6, a->sub == 3 ? TUI_TAB_ON : TUI_TAB_OFF,
               " PVT ");
    if (side)
        tui_split_h(r, r.w - 46, &r, &pane);

    if (!a->have[a->sub]) {
        tui_printf(t, 1, 6, t->w - 2, TUI_BAD, "%s: %s", area_upper[a->sub],
                   a->err[a->sub][0] ? a->err[a->sub] : "no data");
        return;
    }
    human(v->used, used, sizeof used);
    human(v->free, freed, sizeof freed);
    human(v->foot, mapped, sizeof mapped);
    if (has_total(v)) {
        human(v->foot + v->unalloc, total, sizeof total);
        (void)snprintf(pcbuf, sizeof pcbuf, "use %d%%", p);
    } else {
        (void)snprintf(total, sizeof total, "-");
        (void)snprintf(pcbuf, sizeof pcbuf, "use -");
    }
    {
        int gx = t->w - 20 - (int)strlen(pcbuf);
        int x = 2;

        x = pair(t, x, 4, "used", used, TUI_ACCENT, gx);
        x = pair(t, x + 2, 4, "free", freed, TUI_PLAIN, gx);
        x = pair(t, x + 2, 4, "mapped", mapped, TUI_PLAIN, gx);
        (void)pair(t, x + 2, 4, "total", total, TUI_PLAIN, gx);
    }
    tui_printf(t, t->w - 19 - (int)strlen(pcbuf), 4, (int)strlen(pcbuf),
               p < 0    ? TUI_META
               : p < 70 ? TUI_GOOD
               : p < 85 ? TUI_WARN
                        : TUI_BAD,
               "%s", pcbuf);
    if (p >= 0)
        tui_progress(t, t->w - 18, 4, 16, p);
    if (folded(a)) {
        a->storage.cols = pvt_fold_cols;
        a->storage.vp = a->pvt_vp;
        tui_table_draw(t, r, &a->storage, a->npvt, pvt_cell, nullptr, a);
    } else {
        tui_table_draw(t, r, &a->storage, a->sub_rows, storage_cell, nullptr,
                       a);
    }
    if (side) {
        if (folded(a) && a->pvt_vp.sel < a->npvt)
            tcb_box(t, a, pane, a->pvt_vp.sel);
        else if (!folded(a) && a->sub_rows > 0)
            subpool_box(t, a, pane, a->sub_order[a->storage.vp.sel]);
    }

    if (a->err[a->sub][0] != '\0') {
        footer_line(t, a->err[a->sub], TUI_BAD);
    } else if (v->unalloc_valid) {
        char ua[16], ul[16], note[96];

        human(v->unalloc, ua, sizeof ua);
        human(v->largest_unalloc, ul, sizeof ul);
        (void)snprintf(note, sizeof note,
                       "unallocated %s in %u blocks, largest %s", ua,
                       v->unalloc_blocks, ul);
        footer_line(t, note, TUI_META);
    }
    if (a->ntrend[a->sub] > 0 && t->w >= 100) {
        int n = a->ntrend[a->sub];
        int x = t->w - 4 - n;

        tui_printf(t, x - 5, t->h - 2, 4, TUI_META, "used");
        tui_sparkline(t, x, t->h - 2, a->trend[a->sub], n, TUI_ACCENT);
    }
}

/** @brief An accent heading with a dim rule filling the row, stopped at the
 * panel border. */
static void section(tui *t, int y, const char *name) {
    int n = (int)strlen(name);

    tui_printf(t, 1, y, n, TUI_ACCENT, "%s", name);
    if (n + 2 < t->w - 1)
        tui_fill(t, n + 2, y, t->w - n - 3, 1, '-', TUI_META);
}

static void draw_system(tui *t, const app *a) {
    const zsys *z = &a->sys;
    char b1[32], b2[32];
    int half = t->w / 2;

    if (!a->have_sys) {
        tui_printf(t, 1, 4, t->w - 2, TUI_BAD, "%s", a->sys_err);
        return;
    }
    human(z->real_k * 1024, b1, sizeof b1);
    section(t, 4, "identity");
    kv(t, 1, 5, half - 1, "system", z->sysname, TUI_ACCENT);
    kv(t, half, 5, t->w - half - 1, "host", z->host, TUI_PLAIN);
    kv(t, 1, 6, half - 1, "release", z->os, TUI_PLAIN);
    kv(t, half, 6, t->w - half - 1, "machine", z->machine, TUI_PLAIN);

    section(t, 8, "ipl");
    if (z->ipl_ok) {
        (void)snprintf(b1, sizeof b1, "%04u-%02u-%02u", z->ipl.year,
                       z->ipl.month, z->ipl.day);
        (void)snprintf(b2, sizeof b2, "%02u:%02u:%02u", z->ipl.hour,
                       z->ipl.minute, z->ipl.second);
        kv(t, 1, 9, half - 1, "date", b1, TUI_PLAIN);
        kv(t, half, 9, t->w - half - 1, "time", b2, TUI_PLAIN);
    }

    section(t, 11, "storage");
    human(z->real_k * 1024, b1, sizeof b1);
    kv(t, 1, 12, half - 1, "real", b1, TUI_PLAIN);
    (void)snprintf(b2, sizeof b2, "%d used / %u slots", z->njobs, z->slots);
    kv(t, half, 12, t->w - half - 1, "address spaces", b2, TUI_PLAIN);
    if (a->info[0].unalloc_valid) {
        human(a->info[0].unalloc, b1, sizeof b1);
        kv(t, 1, 13, half - 1, "csa unallocated", b1, TUI_PLAIN);
    }
    if (a->info[3].unalloc_valid) {
        human(a->info[3].unalloc, b1, sizeof b1);
        kv(t, half, 13, t->w - half - 1, "pvt unallocated", b1, TUI_PLAIN);
    }
}

static void draw_paging(tui *t, const app *a) {
    const zpage *p = &a->page;
    char b[48];
    int half = t->w / 2;

    if (!a->have_page) {
        tui_printf(t, 1, 4, t->w - 2, TUI_BAD, "%s", a->page_err);
        return;
    }
    section(t, 4, "auxiliary storage");
    (void)snprintf(b, sizeof b, "%08X", (unsigned int)p->asmvt);
    kv(t, 1, 5, half - 1, "asmvt", b, TUI_META);

    section(t, 7, "page data sets");
    (void)snprintf(b, sizeof b, "%s%s", p->common_pds ? "yes" : "no",
                   p->common_full ? ", full" : "");
    kv(t, 1, 8, half - 1, "common", b,
       p->common_full  ? TUI_BAD
       : p->common_pds ? TUI_GOOD
                       : TUI_WARN);
    (void)snprintf(b, sizeof b, "%s%s", p->plpa_pds ? "yes" : "no",
                   p->plpa_full ? ", full" : "");
    kv(t, half, 8, t->w - half - 1, "plpa", b,
       p->plpa_full  ? TUI_BAD
       : p->plpa_pds ? TUI_GOOD
                     : TUI_WARN);
    kv(t, 1, 9, half - 1, "local paging", p->local_paging ? "on" : "off",
       p->local_paging ? TUI_GOOD : TUI_WARN);
    (void)snprintf(b, sizeof b, "%s%s", p->quick_start ? "quick" : "cold",
                   p->warm_start ? ", warm VIO" : "");
    kv(t, half, 9, t->w - half - 1, "start", b,
       p->quick_start || p->warm_start ? TUI_WARN : TUI_PLAIN);

    section(t, 11, "vio journal");
    kv(t, 1, 12, half - 1, "state", p->vio_journaling ? "active" : "off",
       p->vio_journaling ? TUI_GOOD : TUI_META);
    (void)snprintf(b, sizeof b, "%u", p->vio_ci);
    kv(t, half, 12, t->w - half - 1, "control intervals", b, TUI_PLAIN);
}

static void draw_jobs(tui *t, app *a) {
    tui_rect r = table_rect(t->w, t->h, 2);
    char head[24];
    tui_col cols[3];

    if (a->have_sys) {
        int n = 0;

        for (int i = 0; i < a->sys.njobs; i++) {
            char key[24];

            (void)snprintf(key, sizeof key, "%u %s", a->sys.job[i].asid,
                           a->sys.job[i].name);
            if (match(key, a->filter))
                a->job_order[n++] = i;
        }
        a->job_rows = n;
        sort_sys = &a->sys;
        sort_key = a->job_sort;
        sort_dir = a->job_dir;
        qsort(a->job_order, (size_t)n, sizeof a->job_order[0], cmp_job);
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
    tui_table_draw(t, r, &a->jobs, a->job_rows, job_cell, nullptr, a);
    {
        char note[64];

        (void)snprintf(note, sizeof note, "%d address spaces, %u slots",
                       a->sys.njobs, a->sys.slots);
        footer_line(t, note, TUI_LABEL);
    }
}

/** @brief Everything about the selected subpool, over the table. */
/* One subpool, painted into whatever rectangle is handed in: the popup and
 * the inspector pane share this body. */
static void subpool_box(tui *t, const app *a, tui_rect r, int idx) {
    const vsm_info *v = &a->info[a->sub];
    const vsm_subpool *s = &v->sub[idx];
    char key[16], blocks[16], used[16], freed[16], mapped[16], largest[16];
    char below[16], above[16], where[32], title[32];
    const char *k[9];
    const char *val[9];
    int n = 0;

    human(s->used, used, sizeof used);
    human(s->free, freed, sizeof freed);
    human(s->foot, mapped, sizeof mapped);
    human(s->largest, largest, sizeof largest);
    human(s->below, below, sizeof below);
    human(s->above, above, sizeof above);
    (void)snprintf(key, sizeof key, "%u", s->key);
    (void)snprintf(blocks, sizeof blocks, "%u", s->blocks);
    if (v->area == VSM_PVT)
        (void)snprintf(where, sizeof where, "%08X", (unsigned int)s->tcb);
    else
        (void)snprintf(where, sizeof where, "%08X-%08X", (unsigned int)s->lo,
                       (unsigned int)s->hi);
    k[n] = "key", val[n++] = key;
    k[n] = "blocks", val[n++] = blocks;
    k[n] = "used", val[n++] = used;
    k[n] = "below 16M", val[n++] = below;
    k[n] = "above 16M", val[n++] = above;
    k[n] = "free", val[n++] = freed;
    k[n] = "mapped", val[n++] = mapped;
    k[n] = "largest", val[n++] = largest;
    k[n] = v->area == VSM_PVT ? "tcb" : "address", val[n++] = where;

    (void)snprintf(title, sizeof title, "subpool %u", s->id);
    tui_fill(t, r.x, r.y, r.w, r.h, ' ', TUI_PLAIN);
    tui_box_frame(t, r.x, r.y, r.w, r.h, TUI_FRAME, title, TUI_ACCENT);
    for (int i = 0; i < n && r.y + 2 + i < r.y + r.h - 1; i++)
        kv(t, r.x + 2, r.y + 2 + i, r.w - 4, k[i], val[i], TUI_PLAIN);
    if (r.h >= 4)
        tui_printf(t, r.x + 2, r.y + r.h - 2, r.w - 4, TUI_META,
                   "any key closes");
}

/* One folded TCB row: its totals, then the subpools that make them up. */
static void tcb_box(tui *t, const app *a, tui_rect r, int row) {
    const vsm_info *v = &a->info[3];
    const pvt_row *p = &a->pvt[row];
    char used[16], below[16], above[16], subs[16], title[40];
    int line;

    human(p->used, used, sizeof used);
    human(p->below, below, sizeof below);
    human(p->above, above, sizeof above);
    (void)snprintf(subs, sizeof subs, "%u", p->nsub);
    (void)snprintf(title, sizeof title, "TCB %08X", (unsigned int)p->tcb);
    tui_fill(t, r.x, r.y, r.w, r.h, ' ', TUI_PLAIN);
    tui_box_frame(t, r.x, r.y, r.w, r.h, TUI_FRAME, title, TUI_ACCENT);
    kv(t, r.x + 2, r.y + 2, r.w - 4, "subpools", subs, TUI_PLAIN);
    kv(t, r.x + 2, r.y + 3, r.w - 4, "used", used, TUI_PLAIN);
    kv(t, r.x + 2, r.y + 4, r.w - 4, "below 16M", below, TUI_PLAIN);
    kv(t, r.x + 2, r.y + 5, r.w - 4, "above 16M", above, TUI_PLAIN);
    line = r.y + 7;
    if (line < r.y + r.h - 1)
        tui_printf(t, r.x + 2, line++, r.w - 4, TUI_LABEL, "subpools");
    for (int i = 0; i < v->nsub && line < r.y + r.h - 1; i++) {
        const vsm_subpool *s = &v->sub[i];
        char u[16], b[16], ab[16];

        if (s->tcb != p->tcb)
            continue;
        human(s->used, u, sizeof u);
        human(s->below, b, sizeof b);
        human(s->above, ab, sizeof ab);
        tui_printf(t, r.x + 2, line++, r.w - 4, TUI_PLAIN, "%2u %8s %s%s",
                   s->id, u, b[0] == '0' ? "" : "<16M ",
                   ab[0] == '0' ? "" : ">16M");
    }
    if (r.h >= 4)
        tui_printf(t, r.x + 2, r.y + r.h - 2, r.w - 4, TUI_META,
                   "any key closes");
}

static void draw_popup(tui *t, app *a) {
    tui_rect r = {(t->w - 46) / 2, (t->h - 13) / 2, 46, 13};
    const vsm_info *v = &a->info[a->sub];

    if (r.x < 0)
        r.x = 0;
    if (r.y < 0)
        r.y = 0;
    if (a->tcb_popup) {
        if (a->pvt_vp.sel < a->npvt)
            tcb_box(t, a, r, a->pvt_vp.sel);
    } else if (v->nsub > 0) {
        subpool_box(t, a, r, a->sub_order[a->storage.vp.sel]);
    }
}

static void draw_help(tui *t) {
    tui_keymap_help(t, keymap, KEYMAP_COUNT, "keys");
}

/* A filter edit re-runs the orders, because every reader indexes through
 * them: the table, the detail line, the popup and the mouse. */
static void filter_changed(app *a) {
    (void)snprintf(a->filter, sizeof a->filter, "%s", a->fin.buf);
    order_storage(a);
    build_pvt(a);
    a->storage.vp.sel = 0;
    a->storage.vp.top = 0;
    a->pvt_vp.sel = 0;
    a->pvt_vp.top = 0;
    a->jobs.vp.sel = 0;
    a->jobs.vp.top = 0;
}

static void run_command(app *a, const char *cmd) {
    char c[32];
    char *p = c;
    size_t n;

    (void)snprintf(c, sizeof c, "%s", cmd);
    for (char *q = c; *q != '\0'; q++)
        *q = (char)tolower((unsigned char)*q);
    while (*p == ' ')
        p++;
    n = strlen(p);
    while (n > 0 && p[n - 1] == ' ')
        p[--n] = '\0';

    if (strcmp(p, "csa") == 0 || strcmp(p, "sqa") == 0 ||
        strcmp(p, "lsqa") == 0 || strcmp(p, "pvt") == 0) {
        a->tab = 0;
        if (p[0] == 'c')
            a->sub = 0;
        else if (p[0] == 'l')
            a->sub = 2;
        else if (p[0] == 'p')
            a->sub = 3;
        else
            a->sub = 1;
    } else if (strcmp(p, "sys") == 0) {
        a->tab = 1;
    } else if (strcmp(p, "jobs") == 0 || strcmp(p, "da") == 0) {
        a->tab = 2;
    } else if (strcmp(p, "page") == 0) {
        a->tab = 3;
    } else if (strcmp(p, "ps") == 0) {
        a->tab = 4;
        a->have_ps = uss_ps_read(&a->ps);
    } else if (strcmp(p, "fs") == 0) {
        a->tab = 5;
        a->have_fs = uss_fs_read(&a->fs);
    } else if (strcmp(p, "fold") == 0) {
        a->tab = 0;
        a->sub = 3;
        a->pvt_fold = !a->pvt_fold;
    } else if (strcmp(p, "log") == 0) {
        a->tab = 6;
        if (!a->log_on)
            log_start(a);
        log_poll(a);
    } else if (strcmp(p, "pause") == 0) {
        a->paused = !a->paused;
    } else if (strcmp(p, "help") == 0) {
        a->help = true;
    } else if (strcmp(p, "quit") == 0 || strcmp(p, "q") == 0) {
        a->tapp->quit = true;
    }
}

static const tui_col ps_cols[] = {
    {"PID", 9, true}, {"PPID", 9, true}, {"UID", 7, true},
    {"ST", 3, false}, {"CPU", 9, true},  {"COMMAND", 44, false},
};

static const tui_col fs_cols[] = {
    {"MOUNT", 28, false}, {"TYPE", 9, false}, {"SIZE", 9, true},
    {"FREE", 9, true},    {"USED", 5, true},  {"RO", 2, false},
    {"FS", 45, false},
};

static void ps_cell(void *ctx, int row, int col, char *buf, size_t len) {
    const app *a = ctx;
    const uss_proc *p;

    if (row >= a->ps.nproc) {
        buf[0] = '\0';
        return;
    }
    p = &a->ps.proc[row];
    switch (col) {
    case 0:
        (void)snprintf(buf, len, "%d", p->pid);
        break;
    case 1:
        (void)snprintf(buf, len, "%d", p->ppid);
        break;
    case 2:
        (void)snprintf(buf, len, "%d", p->uid);
        break;
    case 3:
        (void)snprintf(buf, len, "%c%s", p->st, p->swapped ? "S" : "");
        break;
    case 4:
        (void)snprintf(buf, len, "%lu.%02lu", p->cpu_ms / 1000,
                       (p->cpu_ms % 1000) / 10);
        break;
    default:
        (void)snprintf(buf, len, "%s", p->cmd);
        break;
    }
}

static void fs_cell(void *ctx, int row, int col, char *buf, size_t len) {
    const app *a = ctx;
    const uss_fs *f;
    char b[16];

    if (row >= a->fs.nfs) {
        buf[0] = '\0';
        return;
    }
    f = &a->fs.fs[row];
    switch (col) {
    case 0:
        (void)snprintf(buf, len, "%s", f->mount);
        break;
    case 1:
        (void)snprintf(buf, len, "%s", f->type);
        break;
    case 2:
        human(f->kb_total * 1024, b, sizeof b);
        (void)snprintf(buf, len, "%s", f->ok ? b : "-");
        break;
    case 3:
        human(f->kb_free * 1024, b, sizeof b);
        (void)snprintf(buf, len, "%s", f->ok ? b : "-");
        break;
    case 4:
        if (f->pct < 0)
            (void)snprintf(buf, len, "-");
        else
            (void)snprintf(buf, len, "%d%%", f->pct);
        break;
    case 5:
        (void)snprintf(buf, len, "%s", f->rdonly ? "ro" : "rw");
        break;
    default:
        (void)snprintf(buf, len, "%s", f->fsname);
        break;
    }
}

static void draw_ps(tui *t, app *a) {
    tui_rect r = table_rect(t->w, t->h, 4);
    char note[64];

    if (!a->have_ps) {
        tui_printf(t, 1, 4, t->w - 2, TUI_BAD, "%s", a->ps.err);
        return;
    }
    tui_table_draw(t, r, &a->pst, a->ps.nproc, ps_cell, nullptr, a);
    (void)snprintf(note, sizeof note, "%d processes%s", a->ps.nproc,
                   a->ps.truncated ? ", more not shown" : "");
    footer_line(t, note, TUI_LABEL);
}

static void draw_fs(tui *t, app *a) {
    tui_rect r = table_rect(t->w, t->h, 5);
    char note[96];

    if (!a->have_fs) {
        tui_printf(t, 1, 4, t->w - 2, TUI_BAD, "%s", a->fs.err);
        return;
    }
    tui_table_draw(t, r, &a->fst, a->fs.nfs, fs_cell, nullptr, a);
    (void)snprintf(note, sizeof note, "%d file systems%s", a->fs.nfs,
                   a->fs.truncated ? ", more not shown" : "");
    footer_line(t, note, TUI_LABEL);
}

/* ---- the console log ---- */

/* One line of the ring.  A line that arrives when the ring is full
 * overwrites the oldest, which is what a console does. */
static void log_put(app *a, const char *line, int len) {
    char *dst = a->log[a->log_next];

    if (len > LOG_COLS - 1)
        len = LOG_COLS - 1;
    if (len < 0)
        len = 0;
    (void)memcpy(dst, line, (size_t)len);
    dst[len] = '\0';
    a->log_next = (a->log_next + 1) % LOG_LINES;
    if (a->nlog < LOG_LINES)
        a->nlog++;
}

/* Line i, counted from the oldest kept. */
static const char *log_at(const app *a, int i) {
    return a->log[(a->log_next + LOG_LINES - a->nlog + i) % LOG_LINES];
}

/* The object walk lives in src/mdb.c: it parses bytes from another
 * address space and has a test of its own. */
static void log_mdb(app *a) {
    char job[16];
    char text[MDB_TEXT_MAX];
    char line[LOG_COLS];
    int n;

    if (!mdb_line(a->mdb, a->mdb_len, job, sizeof job, text, sizeof text))
        return;
    n = snprintf(line, sizeof line, "%-8s %s", job, text);
    if (n < 0)
        return;
    if (n > (int)sizeof line - 1)
        n = (int)sizeof line - 1;
    log_put(a, line, n);
}

/* Take the console down.  Called on quit, and safe to call when it was
 * never up. */
static void log_stop(app *a) {
    if (!a->log_on)
        return;
    a->creq.op = ZMCS_DEACTIVATE;
    a->creq.consid = &a->creq_consid;
    (void)zmcs(&a->creq);
    a->log_on = false;
}

/* Open the console.  A refusal is not fatal: the other views do not need
 * it, so the reason is kept and shown on the tab. */
/* The console stub keeps the MCSOPER control blocks in its own CSECT,
 * which is read-only in this link: it has to be built as a 31-bit module
 * and thunked to (src/zmcs.s says why).  Until that module exists, -c
 * reports it instead of taking a 0C4 on the first privileged call. */
static bool console_built(void) {
    return false;
}

static void log_start(app *a) {
    if (!console_opt) {
        (void)snprintf(a->log_err, sizeof a->log_err,
                       "console off: ztop -c after make auth");
        return;
    }
    if (!console_built()) {
        (void)snprintf(a->log_err, sizeof a->log_err,
                       "console: the stub is not a 31-bit module yet");
        return;
    }
    a->creq = (zmcs_req){};
    console_name(a->creq_name);
    a->creq.op = ZMCS_ACTIVATE;
    a->creq.name = a->creq_name;
    a->creq.consid = &a->creq_consid;
    (void)zmcs(&a->creq);
    if (a->creq.rc > 4) {
        if (a->creq.rc == 12)
            (void)snprintf(a->log_err, sizeof a->log_err,
                           "console needs APF authorization: MODESET rc %d",
                           a->creq.rc);
        else
            (void)snprintf(a->log_err, sizeof a->log_err,
                           "MCSOPER activate: rc %d rsn %d", a->creq.rc,
                           a->creq.rsn);
        return;
    }
    a->log_on = true;
    a->log_err[0] = '\0';
}

/* Drain what is queued.  Bounded: a console burst must not stall a frame,
 * and one call per tick is the steady state, which costs a single
 * MCSOPMSG. */
static void log_poll(app *a) {
    int i;

    if (!a->log_on)
        return;
    /* QLIMIT bounds the queue at the console end; this bounds the work per
     * refresh, so a burst cannot stall a redraw. */
    a->creq.consid = &a->creq_consid;
    a->creq.buf = a->mdb;
    a->creq.len = &a->mdb_len;
    for (i = 0; i < 200; i++) {
        a->mdb_len = (int)sizeof a->mdb;
        a->creq.op = ZMCS_GETMSG;
        (void)zmcs(&a->creq);
        if (a->creq.rc == 8) /* nothing queued */
            return;
        if (a->creq.rc != 0 && a->creq.rc != 4) {
            (void)snprintf(a->log_err, sizeof a->log_err,
                           "MCSOPMSG: rc %d rsn %d", a->creq.rc, a->creq.rsn);
            log_stop(a);
            return;
        }
        log_mdb(a);
    }
}

static void draw_log(tui *t, app *a) {
    tui_rect r = table_rect(t->w, t->h, 6);
    int rows = table_rows(t->w, t->h, 6);
    char note[64];

    if (!a->log_on) {
        tui_printf(t, 1, 4, t->w - 2, TUI_BAD, "%s",
                   a->log_err[0] != '\0' ? a->log_err
                                         : "the console is not open");
        return;
    }
    tui_viewport_clamp(&a->log_vp, a->nlog, rows - 1);
    for (int i = 0; i < rows - 1 && a->log_vp.top + i < a->nlog; i++) {
        int line = a->log_vp.top + i;

        tui_printf(t, r.x, r.y + 1 + i, r.w,
                   line == a->log_vp.sel ? TUI_TAB_ON : TUI_PLAIN, "%s",
                   log_at(a, line));
    }
    (void)snprintf(note, sizeof note, "%d messages%s", a->nlog,
                   a->nlog >= LOG_LINES ? ", oldest dropped" : "");
    footer_line(t, note, TUI_LABEL);
}

/** @brief The right half of the status line: what is under the cursor. */
static void detail(app *a, char *buf, size_t len) {
    if (a->paused) {
        (void)snprintf(buf, len, "PAUSED");
    } else if (folded(a) && a->pvt_vp.sel < a->npvt) {
        const pvt_row *p = &a->pvt[a->pvt_vp.sel];
        char u[16];

        human(p->used, u, sizeof u);
        (void)snprintf(buf, len, "TCB %08X  %u subpools  %s",
                       (unsigned int)p->tcb, p->nsub, u);
    } else if (a->tab == 0 && a->have[a->sub] && a->sub_rows > 0) {
        const vsm_info *v = &a->info[a->sub];
        const vsm_subpool *sp = &v->sub[a->sub_order[vp_of(a)->sel]];

        if (a->sub == 3)
            (void)snprintf(buf, len, "sp %u  tcb %08X", sp->id,
                           (unsigned int)sp->tcb);
        else if (sp->blocks == 0)
            (void)snprintf(buf, len, "sp %u  empty", sp->id);
        else
            (void)snprintf(buf, len, "sp %u  %08X-%08X", sp->id,
                           (unsigned int)sp->lo, (unsigned int)sp->hi);
    } else if (a->tab == 2 && a->have_sys && a->job_rows > 0) {
        const zjob *j = &a->sys.job[a->job_order[a->jobs.vp.sel]];

        (void)snprintf(buf, len, "asid %u  %s", j->asid, j->name);
    } else if (a->tab == 4 && a->have_ps && a->ps.nproc > 0) {
        const uss_proc *p = &a->ps.proc[a->pst.vp.sel];

        (void)snprintf(buf, len, "pid %d  uid %d  %s", p->pid, p->uid, p->cmd);
    } else if (a->tab == 5 && a->have_fs && a->fs.nfs > 0) {
        const uss_fs *f = &a->fs.fs[a->fst.vp.sel];

        if (f->ok)
            (void)snprintf(buf, len, "%s  %llu%% used", f->mount,
                           (unsigned long long)(f->pct < 0 ? 0 : f->pct));
        else
            (void)snprintf(buf, len, "%s  statvfs errno %d", f->mount, f->eno);
    } else {
        buf[0] = '\0';
    }
}

static void view(void *model, tui *t) {
    static const char *const title[7] = {
        "STORAGE",   "SYSTEM",       "JOBS",    "PAGING",
        "PROCESSES", "FILE SYSTEMS", "CONSOLE",
    };
    app *a = model;
    char right[48];

    tui_clear(t);
    draw_header(t, a);
    draw_tabs(t, a);
    /* The tables draw a clamped copy, so the model's selection is brought
     * into range here, before the detail lines read it. */
    tui_viewport_clamp(&a->storage.vp, a->info[a->sub].nsub,
                       table_rows(t->w, t->h, 0));
    tui_viewport_clamp(&a->jobs.vp, a->sys.njobs, table_rows(t->w, t->h, 2));
    tui_viewport_clamp(&a->pst.vp, a->ps.nproc, table_rows(t->w, t->h, 4));
    tui_viewport_clamp(&a->fst.vp, a->fs.nfs, table_rows(t->w, t->h, 5));
    tui_viewport_clamp(&a->log_vp, a->nlog, table_rows(t->w, t->h, 6));
    tui_viewport_clamp(&a->pvt_vp, a->npvt, table_rows(t->w, t->h, 0));
    if (t->w >= 70 && t->h >= 16)
        tui_box_frame(t, 0, 2, t->w, t->h - 3, TUI_FRAME, title[a->tab],
                      TUI_ACCENT);

    if (t->w < 70 || t->h < 16) {
        tui_printf(t, 1, 3, 0, TUI_BAD, "terminal too small: need 70x16");
    } else if (a->tab == 0) {
        draw_storage(t, a);
    } else if (a->tab == 1) {
        draw_system(t, a);
    } else if (a->tab == 2) {
        draw_jobs(t, a);
    } else if (a->tab == 4) {
        draw_ps(t, a);
    } else if (a->tab == 5) {
        draw_fs(t, a);
    } else if (a->tab == 6) {
        draw_log(t, a);
    } else {
        draw_paging(t, a);
    }
    if (a->popup)
        draw_popup(t, a);
    if (a->help)
        draw_help(t);
    detail(a, right, sizeof right);
    tui_statusbar(t, nullptr, right);
    if (a->filtering) {
        tui_put(t, 0, t->h - 1, "/", 1, TUI_KEYCAP);
        tui_input_draw(t, (tui_rect){1, t->h - 1, t->w - 1, 1}, &a->fin,
                       TUI_BAR);
    } else if (a->commanding) {
        tui_put(t, 0, t->h - 1, ":", 1, TUI_KEYCAP);
        tui_input_draw(t, (tui_rect){1, t->h - 1, t->w - 1, 1}, &a->cin,
                       TUI_BAR);
    } else {
        tui_keymap_footer(t, 0, t->h - 1, t->w - 20, keymap, KEYMAP_COUNT,
                          a->tab);
    }
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
        "  -c  open the MCS console for the LOG view (needs APF, see\n"
        "      make auth; an unauthorized program ends with S047)\n"
        "  -h  this text\n");
}

static void wheel(app *a, int dir, int height) {
    if (a->tab == 0 || a->tab == 2 || a->tab == 6)
        tui_viewport_move(vp_of(a), dir, vrows(a), height);
}

/** @brief Tabs, sub-tabs, table rows and the wheel. */
static bool mouse_event(tui_app *ui, const tui_msg *m) {
    app *a = ui->model;
    const tui_mouse *mo = &m->mouse;
    tui_rect r = table_rect(m->w, m->h, a->tab);
    int idx, row;

    if (mo->button == 64 || mo->button == 65) {
        wheel(a, mo->button == 64 ? -1 : 1, table_rows(m->w, m->h, a->tab));
        return true;
    }
    if (mo->release || mo->button != 0)
        return false;
    if (mo->y == 1) {
        idx = tui_tabs_hit(mo->x, tab_labels, 7);
        if (idx < 0)
            return false;
        a->tab = idx;
        return true;
    }
    if (a->tab == 0 && mo->y == r.y - 2) {
        /* The drawn labels, without their trailing space. */
        static const tui_rect area[4] = {
            {1, 0, 5, 1}, {7, 0, 5, 1}, {13, 0, 6, 1}, {20, 0, 5, 1}};

        for (int i = 0; i < 4; i++)
            if (tui_rect_hit((tui_rect){area[i].x, area[i].y + r.y - 2,
                                        area[i].w, area[i].h},
                             mo->x, mo->y)) {
                a->sub = i;
                return true;
            }
        return false;
    }
    if (!tui_rect_hit(r, mo->x, mo->y) || mo->y == r.y)
        return false;
    row = vp_of(a)->top + (mo->y - r.y - 1);
    if (row < vrows(a)) {
        vp_of(a)->sel = row;
        return true;
    }
    return false;
}

static bool update(tui_app *ui, const tui_msg *m) {
    app *a = ui->model;
    int height;

    if (a->help) {
        if (m->type == TUI_MSG_KEY && (m->key == '?' || m->key == TUI_KEY_ESC ||
                                       m->key == 'q' || m->key == 0x03))
            a->help = false;
        return true;
    }
    if (a->popup) {
        if (m->type == TUI_MSG_KEY) {
            a->popup = false;
            a->tcb_popup = false;
        }
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
        return mouse_event(ui, m);
    if (m->type != TUI_MSG_KEY)
        return false;

    if (a->filtering || a->commanding) {
        tui_input *in = a->filtering ? &a->fin : &a->cin;

        if (m->key == TUI_KEY_ESC || m->key == 0x03) {
            if (a->filtering) {
                a->fin = (tui_input){};
                filter_changed(a);
            }
            a->cin = (tui_input){};
            a->filtering = false;
            a->commanding = false;
        } else if (m->key == '\r' || m->key == '\n') {
            if (a->commanding)
                run_command(a, a->cin.buf);
            a->cin = (tui_input){};
            a->filtering = false;
            a->commanding = false;
        } else {
            (void)tui_input_key(in, m->key);
            if (a->filtering)
                filter_changed(a);
        }
        return true;
    }

    if (m->key == '/' || m->key == ':') {
        tui_input *in = m->key == '/' ? &a->fin : &a->cin;

        if (m->key == '/')
            a->filtering = true;
        else
            a->commanding = true;
        *in = (tui_input){};
        in->focus = true;
        return true;
    }

    if (m->key == 'q' || m->key == TUI_KEY_ESC || m->key == 0x03) {
        log_stop(a);
        ui->quit = true;
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
    if (m->key == 't' && a->tab == 0 && a->sub == 3) {
        a->pvt_fold = !a->pvt_fold;
        return true;
    }
    if (m->key == 'i') {
        a->inspector = !a->inspector;
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
        a->tab = (a->tab + 1) % 7;
        return true;
    }
    if (m->key == TUI_KEY_BACKTAB) {
        a->tab = (a->tab + 6) % 7;
        return true;
    }
    if (m->key >= '1' && m->key <= '7') {
        if (a->tab == 0 && m->key <= '4') { /* 1-4 are the areas here */
            a->sub = m->key - '1';
            a->storage.vp.sel = 0;
            a->storage.vp.top = 0;
            a->pvt_vp.sel = 0;
            a->pvt_vp.top = 0;
            return true;
        }
        a->tab = m->key - '1';
        if (a->tab == 4)
            a->have_ps = uss_ps_read(&a->ps);
        else if (a->tab == 5)
            a->have_fs = uss_fs_read(&a->fs);
        else if (a->tab == 6) {
            if (!a->log_on)
                log_start(a);
            log_poll(a);
        }
        return true;
    }
    if ((m->key == 's' || m->key == 'S') && (a->tab == 0 || a->tab == 2)) {
        int *key = a->tab == 0 ? &a->sub_sort : &a->job_sort;
        int *dir = a->tab == 0 ? &a->sub_dir : &a->job_dir;
        int n = a->tab == 0 ? SS_COUNT : JS_COUNT;

        if (m->key == 's')
            *key = (*key + 1) % n;
        else
            *dir = -*dir;
        return true;
    }
    if (a->tab == 0 && (m->key == '\r' || m->key == '\n') && vrows(a) > 0) {
        a->popup = true;
        a->tcb_popup = folded(a);
        return true;
    }

    height = vheight(a, m->h);
    if (height < 1)
        height = 1;
    if (a->tab == 0 || a->tab == 2 || a->tab == 6)
        return tui_viewport_key(vp_of(a), m->key, vrows(a), height);
    return false;
}

static void sys_fill(zsys *z, const char *host) {
    struct utsname u;

    if (uname(&u) == 0) {
        (void)snprintf(z->os, sizeof z->os, "%s %s", u.sysname, u.release);
        (void)snprintf(z->machine, sizeof z->machine, "%s", u.machine);
    }
    (void)snprintf(z->host, sizeof z->host, "%s", host);
}

static void run_tui(int interval) {
    static app a;
    tui t;
    tui_app ta;

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
    a.pst.cols = ps_cols;
    a.pst.ncols = 6;
    a.fst.cols = fs_cols;
    a.fst.ncols = 7;

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
    log_stop(&a); /* a signal can end the loop without the q path */
    tui_close(&t);
}

static void run_oneshot(unsigned mask, bool want_sys, bool want_jobs,
                        bool want_page, bool dump) {
    char host[65];
    bool printed = false;
    int rc = 0;
    int i;

    for (i = 0; i < 4; i++) {
        vsm_info v;

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
        zpage p;

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
        zsys z;

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
        } else if (strcmp(argv[i], "-c") == 0) {
            console_opt = true;
        } else if (strcmp(argv[i], "-d") == 0) {
            interactive = true;
            if (i + 1 < argc && argv[i + 1][0] >= '0' &&
                argv[i + 1][0] <= '9') {
                long secs = strtol(argv[++i], nullptr, 10);

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
