/**
 * @file ztop.c
 * @brief Storage overview for z/OS: one-shot reports and a full-screen view.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "tui.h"
#include "vsm.h"

#define ZTOP_VERSION "0.2"
#define WORK_SIZE (1024u * 1024u)

#if defined(__MVS__)
/* HLASM upcases the ESD name of the CSECT, so the assembler side is
 * exported as ZVSLIST whatever case the source is written in. */
extern int zvslist(void *) __asm__("ZVSLIST");
#pragma linkage(zvslist, OS)

/* The 31-bit request block.  R13 is saved by the stub, so the save area has
 * to live in storage the C compiler gives us, not in the read-only CSECT. */
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

static const char *const area_name[] = { "csa", "sqa", "lsqa", "pvt" };
static const char *const area_upper[] = { "CSA", "SQA", "LSQA", "PVT" };

static void human(uint64_t n, char *buf, size_t len)
{
    if (n >= (1ULL << 30))
        (void)snprintf(buf, len, "%.1fG", (double)n / (double)(1ULL << 30));
    else if (n >= (1ULL << 20))
        (void)snprintf(buf, len, "%.1fM", (double)n / (double)(1ULL << 20));
    else if (n >= (1ULL << 10))
        (void)snprintf(buf, len, "%.1fK", (double)n / (double)(1ULL << 10));
    else
        (void)snprintf(buf, len, "%llu", (unsigned long long)n);
}

#if defined(__MVS__)
static void hexdump(const char *tag, const unsigned char *p, unsigned len,
                    unsigned max)
{
    if (len > max)
        len = max;
    for (unsigned i = 0; i < len; i += 16) {
        (void)fprintf(stderr, "%s %04X:", tag, i);
        for (unsigned j = 0; j < 16 && i + j < len; j++)
            (void)fprintf(stderr, " %02X", p[i + j]);
        (void)fputc('\n', stderr);
    }
}

static void dump_work(const struct vsm_info *v, const unsigned char *workp,
                      const unsigned char *data, unsigned len, int rc)
{
    (void)fprintf(stderr, "ztop: VSMLIST %s %s rc=%d data=%08X len=%u\n",
                  area_name[v->area - 1],
                  v->space == VSM_UNALLOC ? "UNALLOC" : "FREE", rc,
                  (unsigned)(uintptr_t)data, len);
    hexdump("work", workp, 32, 32);
    if (data != NULL && len > 0)
        hexdump("data", data, len, 256);
}
#endif

static int pct(const struct vsm_info *v)
{
    uint64_t total;

    if (!v->unalloc_valid || v->area != VSM_CSA)
        return -1;
    total = v->foot + v->unalloc;
    if (total == 0)
        return 0;
    return (int)((v->used * 100u + total / 2) / total);
}

#if defined(__MVS__)
static bool fetch_one(struct vsm_info *v, int area, int space, bool dump)
{
    int calls = 0;

    vsm_reset(v, (unsigned)area, (unsigned)space);
    memset(work, 0, 4);                  /* return code must start at 0 */
    for (;;) {
        if (++calls > 64) {
            (void)snprintf(v->err, sizeof v->err,
                           "VSMLIST did not finish after 64 calls");
            return false;
        }
        struct zreq req = { 0 };
        req.area = area;
        req.space = space;
        req.len = WORK_SIZE;
        req.wk = work;
        req.rc = -1;
        (void)zvslist(&req);
        if (req.rc != 0 && req.rc != 4) {
            (void)snprintf(v->err, sizeof v->err,
                           "VSMLIST returned %d", req.rc);
            return false;
        }
        const unsigned char *data;
        unsigned len;
        if (!vsm_work_data(work, WORK_SIZE, &data, &len)) {
            (void)snprintf(v->err, sizeof v->err,
                           "VSMLIST work area has an unusable data pointer");
            return false;
        }
        if (dump)
            dump_work(v, work, data, len, req.rc);
        bool ok = space == VSM_UNALLOC
                      ? vsm_parse_unalloc(v, data, len)
                      : vsm_parse_alloc(v, data, len);
        if (!ok)
            return false;
        if (req.rc == 0)
            return true;
        /* rc 4: the service wants the same request and work area again. */
    }
}
#else
static bool fetch_one(struct vsm_info *v, int area, int space, bool dump)
{
    (void)area;
    (void)space;
    (void)dump;
    (void)snprintf(v->err, sizeof v->err, "VSMLIST is only available on z/OS");
    return false;
}
#endif

static bool collect_area(struct vsm_info *v, int area, bool dump)
{
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

static void print_area(const struct vsm_info *v)
{
    const char *name = area_name[v->area - 1];
    bool pvt = v->area == VSM_PVT;
    int p = pct(v);

    (void)printf("area %s: used %llu free %llu mapped %llu blocks %u",
                 name, (unsigned long long)v->used,
                 (unsigned long long)v->free,
                 (unsigned long long)v->foot, v->blocks);
    if (v->area == VSM_CSA && v->unalloc_valid)
        (void)printf(" unallocated %llu total %llu use %d%%",
                     (unsigned long long)v->unalloc,
                     (unsigned long long)(v->foot + v->unalloc), p);
    if (pvt)
        (void)printf(" tcbs %u", v->tcbs);
    (void)printf("\n");

    if (pvt)
        (void)printf("%3s %3s %8s %5s %10s %10s %10s %10s %8s\n",
                     "sp", "key", "tcb", "blks", "used", "free", "mapped",
                     "largest", "addr");
    else
        (void)printf("%3s %3s %6s %10s %10s %10s %10s %17s\n",
                     "sp", "key", "blks", "used", "free", "mapped",
                     "largest", "address");

    for (int i = 0; i < v->nsub; i++) {
        const struct vsm_subpool *s = &v->sub[i];
        char addr[32];

        if (s->blocks == 0)
            (void)snprintf(addr, sizeof addr, "-");
        else if (pvt)
            (void)snprintf(addr, sizeof addr, "%08X", (unsigned)s->lo);
        else
            (void)snprintf(addr, sizeof addr, "%08X-%08X",
                           (unsigned)s->lo, (unsigned)s->hi);
        if (pvt)
            (void)printf("%3u %3u %08X %5u %10llu %10llu %10llu %10llu %s\n",
                         s->id, s->key, (unsigned)s->tcb, s->blocks,
                         (unsigned long long)s->used,
                         (unsigned long long)s->free,
                         (unsigned long long)s->foot,
                         (unsigned long long)s->largest, addr);
        else
            (void)printf("%3u %3u %6u %10llu %10llu %10llu %10llu %s\n",
                         s->id, s->key, s->blocks,
                         (unsigned long long)s->used,
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
        for (int i = 0; i < v->nregion && i < VSM_MAX_REGION; i++)
            (void)printf("%s %08X+%08X", i == 0 ? " regions" : ",",
                         (unsigned)v->region[i].addr,
                         (unsigned)v->region[i].len);
        (void)printf("\n");
    }
    if (v->err[0] != '\0')
        (void)printf("warning: %s\n", v->err);
}

struct app {
    struct vsm_info info[4];
    bool have[4];
    char err[4][160];
    int sel;
    bool all;
    bool paused;
    bool help;
    int interval;
    int collect_ms;
    int scroll;
    char host[80];
};

static int visible_rows(const struct tui *t)
{
    int n = t->h - 8;
    return n < 1 ? 1 : n;
}

static void collect(struct app *a)
{
    struct timespec t0, t1;
    int first = a->all ? 0 : a->sel;
    int last = a->all ? 3 : a->sel;

    (void)clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int i = first; i <= last; i++) {
        struct vsm_info v;
        if (collect_area(&v, i + 1, false)) {
            a->info[i] = v;
            a->have[i] = true;
            a->err[i][0] = '\0';
        } else {
            (void)snprintf(a->err[i], sizeof a->err[i], "%s", v.err);
            if (!a->have[i])
                a->info[i].area = (unsigned)(i + 1);
        }
    }
    (void)clock_gettime(CLOCK_MONOTONIC, &t1);
    a->collect_ms = (int)((t1.tv_sec - t0.tv_sec) * 1000 +
                          (t1.tv_nsec - t0.tv_nsec) / 1000000);
}

static void draw_tabs(struct tui *t, const struct app *a)
{
    static const char *const tabs[4] = { "1 CSA", "2 SQA", "3 LSQA", "4 PVT" };

    tui_fill(t, 0, 1, t->w, 1, ' ', TUI_PLAIN);
    for (int i = 0; i < 4; i++) {
        tui_style st = (!a->all && a->sel == i) ? TUI_TITLE : TUI_LABEL;
        tui_printf(t, 1 + i * 9, 1, 8, st, " %s ", tabs[i]);
    }
    tui_printf(t, 38, 1, 7, a->all ? TUI_TITLE : TUI_LABEL, " a ALL ");
    if (a->paused)
        tui_printf(t, t->w - 8, 1, 7, TUI_WARN, " PAUSED");
}

static void draw_bar(struct tui *t, int x, int y, int p)
{
    char b[13];
    int fill = p < 0 ? 0 : p / 10;

    if (fill > 10)
        fill = 10;
    b[0] = '[';
    for (int i = 0; i < 10; i++)
        b[1 + i] = i < fill ? '#' : '-';
    b[11] = ']';
    b[12] = '\0';
    tui_put(t, x, y, b, 12,
            p < 0 ? TUI_STYLE(7, 0, TUI_DIM)
                  : p < 70 ? TUI_GOOD : p < 85 ? TUI_WARN : TUI_BAD);
}

static void draw_all(struct tui *t, const struct app *a)
{
    static const int xnum = 8, xused = 15, xfree = 26, xmap = 37;
    static const int xtot = 48, xpct = 59, xbar = 66;

    tui_printf(t, 1, 3, 6, TUI_LABEL, "AREA");
    tui_printf(t, xnum, 3, 6, TUI_LABEL, "BLOCKS");
    tui_printf(t, xused, 3, 9, TUI_LABEL, "USED");
    tui_printf(t, xfree, 3, 9, TUI_LABEL, "FREE");
    tui_printf(t, xmap, 3, 9, TUI_LABEL, "MAPPED");
    tui_printf(t, xtot, 3, 9, TUI_LABEL, "TOTAL");
    tui_printf(t, xpct, 3, 5, TUI_LABEL, "USE");

    for (int i = 0; i < 4; i++) {
        int y = 4 + i;
        const struct vsm_info *v = &a->info[i];
        char used[16], freed[16], mapped[16], total[16], pc[8];
        int p;

        if (!a->have[i]) {
            tui_printf(t, 1, y, 0, TUI_BAD, "%-4s  %s", area_upper[i],
                       a->err[i][0] ? a->err[i] : "collecting...");
            continue;
        }
        p = pct(v);
        human(v->used, used, sizeof used);
        human(v->free, freed, sizeof freed);
        human(v->foot, mapped, sizeof mapped);
        if (v->area == VSM_CSA && v->unalloc_valid) {
            human(v->foot + v->unalloc, total, sizeof total);
            (void)snprintf(pc, sizeof pc, "%d%%", p);
        } else {
            (void)snprintf(total, sizeof total, "-");
            (void)snprintf(pc, sizeof pc, "-");
        }
        tui_printf(t, 1, y, 4, TUI_PLAIN, "%s", area_upper[i]);
        tui_printf(t, xnum, y, 6, TUI_PLAIN, "%6u", v->blocks);
        tui_printf(t, xused, y, 9, TUI_PLAIN, "%9s", used);
        tui_printf(t, xfree, y, 9, TUI_PLAIN, "%9s", freed);
        tui_printf(t, xmap, y, 9, TUI_PLAIN, "%9s", mapped);
        tui_printf(t, xtot, y, 9, TUI_PLAIN, "%9s", total);
        tui_printf(t, xpct, y, 5,
                   p < 0 ? TUI_PLAIN : p < 70 ? TUI_GOOD :
                   p < 85 ? TUI_WARN : TUI_BAD, "%5s", pc);
        draw_bar(t, xbar, y, p);
    }
}

static void draw_detail(struct tui *t, struct app *a)
{
    const struct vsm_info *v = &a->info[a->sel];
    bool pvt = a->sel == 3;
    int rows = visible_rows(t);
    int p = pct(v);

    if (!a->have[a->sel]) {
        tui_printf(t, 1, 3, 0, TUI_BAD, "%s: %s", area_upper[a->sel],
                   a->err[a->sel][0] ? a->err[a->sel] : "no data");
        return;
    }

    char used[16], freed[16], mapped[16], total[16];
    human(v->used, used, sizeof used);
    human(v->free, freed, sizeof freed);
    human(v->foot, mapped, sizeof mapped);
    if (v->area == VSM_CSA && v->unalloc_valid)
        human(v->foot + v->unalloc, total, sizeof total);
    else
        (void)snprintf(total, sizeof total, "-");

    tui_printf(t, 1, 3, 0, TUI_PLAIN,
               "%s  used %s  free %s  mapped %s  total %s  %d%%",
               area_upper[a->sel], used, freed, mapped, total, p);

    if (pvt) {
        tui_printf(t, 1, 4, 3, TUI_LABEL, "SP");
        tui_printf(t, 5, 4, 3, TUI_LABEL, "KY");
        tui_printf(t, 9, 4, 8, TUI_LABEL, "TCB");
        tui_printf(t, 18, 4, 5, TUI_LABEL, "BLKS");
        tui_printf(t, 25, 4, 9, TUI_LABEL, "USED");
        tui_printf(t, 35, 4, 9, TUI_LABEL, "FREE");
        tui_printf(t, 45, 4, 9, TUI_LABEL, "MAPPED");
        tui_printf(t, 55, 4, 9, TUI_LABEL, "LARGEST");
        tui_printf(t, 65, 4, 9, TUI_LABEL, "ADDR");
    } else {
        tui_printf(t, 1, 4, 3, TUI_LABEL, "SP");
        tui_printf(t, 5, 4, 3, TUI_LABEL, "KY");
        tui_printf(t, 9, 4, 6, TUI_LABEL, "BLKS");
        tui_printf(t, 16, 4, 9, TUI_LABEL, "USED");
        tui_printf(t, 26, 4, 9, TUI_LABEL, "FREE");
        tui_printf(t, 36, 4, 9, TUI_LABEL, "MAPPED");
        tui_printf(t, 46, 4, 9, TUI_LABEL, "LARGEST");
        tui_printf(t, 56, 4, 17, TUI_LABEL, "ADDRESS");
    }

    if (a->scroll > v->nsub - rows)
        a->scroll = v->nsub - rows;
    if (a->scroll < 0)
        a->scroll = 0;

    for (int r = 0; r < rows && a->scroll + r < v->nsub; r++) {
        const struct vsm_subpool *s = &v->sub[a->scroll + r];
        int y = 5 + r;
        char u[16], f[16], m[16], l[16], addr[32];

        human(s->used, u, sizeof u);
        human(s->free, f, sizeof f);
        human(s->foot, m, sizeof m);
        human(s->largest, l, sizeof l);
        if (s->blocks == 0)
            (void)snprintf(addr, sizeof addr, "-");
        else if (pvt)
            (void)snprintf(addr, sizeof addr, "%08X", (unsigned)s->lo);
        else
            (void)snprintf(addr, sizeof addr, "%08X-%08X",
                           (unsigned)s->lo, (unsigned)s->hi);

        tui_printf(t, 1, y, 3, TUI_PLAIN, "%3u", s->id);
        tui_printf(t, 5, y, 3, TUI_PLAIN, "%3u", s->key);
        if (pvt) {
            tui_printf(t, 9, y, 8, TUI_LABEL, "%08X", (unsigned)s->tcb);
            tui_printf(t, 18, y, 5, TUI_PLAIN, "%5u", s->blocks);
            tui_printf(t, 25, y, 9, TUI_PLAIN, "%9s", u);
            tui_printf(t, 35, y, 9, TUI_PLAIN, "%9s", f);
            tui_printf(t, 45, y, 9, TUI_PLAIN, "%9s", m);
            tui_printf(t, 55, y, 9, TUI_PLAIN, "%9s", l);
            tui_printf(t, 65, y, 8, TUI_LABEL, "%s", addr);
        } else {
            tui_printf(t, 9, y, 6, TUI_PLAIN, "%6u", s->blocks);
            tui_printf(t, 16, y, 9, TUI_PLAIN, "%9s", u);
            tui_printf(t, 26, y, 9, TUI_PLAIN, "%9s", f);
            tui_printf(t, 36, y, 9, TUI_PLAIN, "%9s", m);
            tui_printf(t, 46, y, 9, TUI_PLAIN, "%9s", l);
            tui_printf(t, 56, y, 17, TUI_LABEL, "%s", addr);
        }
    }

    /* The line above the keys: first an error, then regions. */
    int y = t->h - 3;
    if (a->err[a->sel][0] != '\0')
        tui_printf(t, 1, y, t->w - 2, TUI_BAD, "! %s", a->err[a->sel]);
    else if (v->unalloc_valid) {
        if (v->nregion > 0) {
            tui_printf(t, 1, y, t->w - 2, TUI_LABEL,
                       "regions: %08X+%08X%s", (unsigned)v->region[0].addr,
                       (unsigned)v->region[0].len,
                       v->nregion > 1 ? " ..." : "");
        } else {
            tui_printf(t, 1, y, t->w - 2, TUI_PLAIN,
                       "unallocated %llu bytes, %u blocks, largest %llu",
                       (unsigned long long)v->unalloc, v->unalloc_blocks,
                       (unsigned long long)v->largest_unalloc);
        }
    }
}

static void draw_help(struct tui *t)
{
    int w = t->w < 56 ? t->w - 2 : 54;
    int h = 15;
    int x = (t->w - w) / 2;
    int y = (t->h - h) / 2;

    tui_fill(t, x, y, w, h, ' ', TUI_PLAIN);
    tui_box(t, x, y, w, h, TUI_LABEL);
    tui_printf(t, x + 2, y + 1, w - 4, TUI_TITLE, "ztop keys");
    tui_printf(t, x + 2, y + 3, w - 4, TUI_PLAIN, "q      quit");
    tui_printf(t, x + 2, y + 4, w - 4, TUI_PLAIN, "1-4    csa, sqa, lsqa, pvt");
    tui_printf(t, x + 2, y + 5, w - 4, TUI_PLAIN, "a      all areas");
    tui_printf(t, x + 2, y + 6, w - 4, TUI_PLAIN, "p      pause refresh");
    tui_printf(t, x + 2, y + 7, w - 4, TUI_PLAIN, "r      refresh now");
    tui_printf(t, x + 2, y + 8, w - 4, TUI_PLAIN, "+ -    interval up / down");
    tui_printf(t, x + 2, y + 9, w - 4, TUI_PLAIN, "j k    scroll subpools");
    tui_printf(t, x + 2, y + 10, w - 4, TUI_PLAIN, "up dn  same");
    tui_printf(t, x + 2, y + 11, w - 4, TUI_PLAIN, "g G    first / last");
    tui_printf(t, x + 2, y + 12, w - 4, TUI_PLAIN, "?      close help");
}

static void draw(struct tui *t, struct app *a)
{
    char now[16];
    time_t clock_now = time(NULL);
    struct tm tm;

    tui_clear(t);
    (void)localtime_r(&clock_now, &tm);
    (void)strftime(now, sizeof now, "%H:%M:%S", &tm);

    tui_fill(t, 0, 0, t->w, 1, ' ', TUI_TITLE);
    tui_printf(t, 1, 0, t->w - 12, TUI_TITLE,
               " ztop %s  %s  %s  refresh %ds  collect %dms ",
               ZTOP_VERSION, a->host, now, a->interval, a->collect_ms);
    draw_tabs(t, a);
    tui_fill(t, 0, 2, t->w, 1, '-', TUI_STYLE(7, 0, TUI_DIM));

    if (t->w < 70 || t->h < 14) {
        tui_printf(t, 1, 3, 0, TUI_BAD, "terminal too small: need 70x14");
    } else if (a->all) {
        draw_all(t, a);
        tui_printf(t, 1, 9, 0, TUI_PLAIN,
                   "unallocated: CSA %llu  PVT %llu",
                   (unsigned long long)(a->info[0].unalloc_valid
                                            ? a->info[0].unalloc : 0),
                   (unsigned long long)(a->info[3].unalloc_valid
                                            ? a->info[3].unalloc : 0));
    } else {
        draw_detail(t, a);
    }

    if (a->help)
        draw_help(t);

    tui_fill(t, 0, t->h - 1, t->w, 1, ' ', TUI_TITLE);
    tui_printf(t, 1, t->h - 1, t->w - 2, TUI_TITLE,
               "q quit  1-4 area  a all  p pause  r refresh  +/- %ds  ? help",
               a->interval);
}

static bool parse_areas(const char *s, unsigned *mask)
{
    unsigned m = 0;

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

static void usage(FILE *f)
{
    (void)fprintf(f,
        "usage: ztop [-a csa|sqa|lsqa|pvt|all] [-x] [-d [seconds]] [-h]\n"
        "  -a  area to report (default csa); comma-separate areas or use all\n"
        "  -x  dump the VSMLIST work and data areas (one-shot only)\n"
        "  -d  interactive full-screen view, refreshed every 2 seconds\n"
        "  -h  this text\n");
}

static void run_oneshot(unsigned mask, bool dump)
{
    int rc = 0;

    for (int i = 0; i < 4; i++) {
        if ((mask & (1u << i)) == 0)
            continue;
        struct vsm_info v;
        if (!collect_area(&v, i + 1, dump)) {
            (void)fprintf(stderr, "ztop: %s: %s\n", area_name[i], v.err);
            rc = 1;
            continue;
        }
        if (i != 0)
            (void)printf("\n");
        print_area(&v);
    }
    exit(rc);
}

static void run_tui(unsigned mask, int interval)
{
    static struct app a;
    struct tui t;

    memset(&a, 0, sizeof a);
    a.interval = interval;
    a.all = (mask & 0xFu) == 0xFu;
    for (int i = 0; i < 4; i++) {
        a.info[i].area = (unsigned)(i + 1);
        if (!a.all && (mask & (1u << i)) != 0 && a.sel == 0)
            a.sel = i;
    }
    if (gethostname(a.host, sizeof a.host - 1) != 0)
        (void)snprintf(a.host, sizeof a.host, "?");
    a.host[sizeof a.host - 1] = '\0';

    if (!tui_open(&t)) {
        (void)fprintf(stderr, "ztop: -d needs a terminal\n");
        exit(1);
    }
    collect(&a);
    for (;;) {
        draw(&t, &a);
        tui_flush(&t);
        int k = tui_key(&t, a.paused ? -1 : a.interval * 1000);

        if (k == TUI_KEY_NONE) {
            collect(&a);
            continue;
        }
        if (k == TUI_KEY_RESIZE) {
            (void)tui_resize(&t);
            continue;
        }
        if (a.help) {
            if (k == '?' || k == TUI_KEY_ESC)
                a.help = false;
            else if (k == 'q')
                break;
            continue;
        }
        if (k == 'q' || k == 0x03 || k == TUI_KEY_ESC)
            break;
        if (k >= '1' && k <= '4') {
            a.sel = k - '1';
            a.all = false;
            a.scroll = 0;
            collect(&a);
            continue;
        }
        if (k == 'a') {
            a.all = !a.all;
            collect(&a);
            continue;
        }
        if (k == 'p') {
            a.paused = !a.paused;
            continue;
        }
        if (k == 'r') {
            collect(&a);
            continue;
        }
        if (k == '+' || k == '=') {
            if (a.interval < 60)
                a.interval++;
            continue;
        }
        if (k == '-') {
            if (a.interval > 1)
                a.interval--;
            continue;
        }
        if (k == '?') {
            a.help = true;
            continue;
        }
        int rows = visible_rows(&t);
        int max = a.info[a.sel].nsub - rows;
        if (max < 0)
            max = 0;
        if (k == 'j' || k == TUI_KEY_DOWN) {
            if (a.scroll < max)
                a.scroll++;
        } else if (k == 'k' || k == TUI_KEY_UP) {
            if (a.scroll > 0)
                a.scroll--;
        } else if (k == TUI_KEY_PGDN) {
            a.scroll += rows;
            if (a.scroll > max)
                a.scroll = max;
        } else if (k == TUI_KEY_PGUP) {
            a.scroll -= rows;
            if (a.scroll < 0)
                a.scroll = 0;
        } else if (k == 'g' || k == TUI_KEY_HOME) {
            a.scroll = 0;
        } else if (k == 'G' || k == TUI_KEY_END) {
            a.scroll = max;
        }
    }
    tui_close(&t);
}

int main(int argc, char **argv)
{
    unsigned mask = 0;
    bool dump = false;
    bool interactive = false;
    int interval = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0) {
            usage(stdout);
            return 0;
        } else if (strcmp(argv[i], "-x") == 0) {
            dump = true;
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
            if (i + 1 < argc && argv[i + 1][0] >= '0' && argv[i + 1][0] <= '9')
                interval = atoi(argv[++i]);
        } else {
            usage(stderr);
            return 2;
        }
    }
    if (mask == 0)
        mask = 1u;

    if (interactive) {
        if (dump)
            (void)fprintf(stderr, "ztop: -x is ignored with -d\n");
        if (interval < 1)
            interval = 2;
        if (interval > 60)
            interval = 60;
        run_tui(mask, interval);
    } else {
        run_oneshot(mask, dump);
    }
    return 0;
}
