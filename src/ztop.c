/**
 * @file ztop.c
 * @brief Show how much storage is allocated in each subpool of a common area.
 *
 * The numbers come from VSMLIST, the one interface z/OS offers an ordinary
 * user task for this (problem state, key 8, LINKAGE=SYSTEM).  It is a
 * macro, so the call lives in src/zvslist.s and this file only parses what
 * comes back.
 *
 *   ztop [-a csa|sqa|lsqa|pvt] [-x]
 *
 * Only z/OS builds: VSMLIST does not exist anywhere else.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/** @brief One request to the assembler stub; layout is shared with zvslist.s. */
struct zreq {
    int area;      /**< 1 CSA, 2 SQA, 3 LSQA, 4 private */
    unsigned len;  /**< bytes in wk */
    void *wk;      /**< work area, VSMLIST writes into it */
    int rc;        /**< the service's return code */
};

/* HLASM upcases the ESD name of the CSECT, so the assembler side is
 * exported as ZVSLIST whatever case the source is written in. */
extern int zvslist(struct zreq *req) __asm__("ZVSLIST");
#pragma linkage(zvslist, OS)

/** @brief Work area size; IBM's minimum is 4K, a busy CSA needs more. */
static constexpr unsigned WORK = 65536;

/** @brief A subpool descriptor (VSM data area, `VSMD`). */
static constexpr unsigned SUBPOOL_DESC = 8;

static unsigned be32(const unsigned char *p) {
    return ((unsigned)p[0] << 24) | ((unsigned)p[1] << 16) | ((unsigned)p[2] << 8) | p[3];
}

/** @brief Storage behind one subpool descriptor. */
struct subpool {
    unsigned id;
    unsigned long long bytes;
    unsigned blocks;
};

static void dump(const unsigned char *p, unsigned len) {
    for (unsigned i = 0; i < len; i += 16) {
        printf("%04X ", i);
        for (unsigned j = 0; j < 16; j++)
            printf(i + j < len ? " %02X" : "   ", p[i + j]);
        printf("  ");
        for (unsigned j = 0; j < 16 && i + j < len; j++)
            printf("%c", p[i + j] >= 0x20 && p[i + j] < 0x7F ? p[i + j] : '.');
        printf("\n");
    }
}

/**
 * @brief Walk the VSMLIST data area and total the allocated bytes per subpool.
 *
 * A subpool descriptor is X'00', then its length, the subpool id, key and
 * flags, and the owning TCB; allocated block descriptors follow it as
 * address/size pairs.  A new subpool descriptor ends the previous run of
 * blocks.
 *
 * shortcut: the descriptor stream has no explicit end marker here, so it
 * is walked until the bytes stop being plausible; a wrong guess shows up as
 * an implausible subpool id rather than as silent corruption, and -x dumps
 * the raw work area to check against.
 */
static int parse(const unsigned char *p, unsigned len, struct subpool *out, int max) {
    int n = 0;
    unsigned off = 4; /* bytes 0-3 are the return code */

    while (off + SUBPOOL_DESC <= len && n < max) {
        if (p[off] != 0x00 || p[off + 1] < SUBPOOL_DESC || p[off + 1] > 64) {
            off++;
            continue;
        }
        unsigned len_desc = p[off + 1];
        struct subpool *s = &out[n++];
        s->id = p[off + 2];
        s->bytes = 0;
        s->blocks = 0;
        off += len_desc;

        while (off + 8 <= len && p[off] != 0x00) {
            unsigned long long size = be32(p + off + 4) & 0x7FFFFFF8u;
            if (size == 0 || size > (4ull << 30))
                break;
            s->bytes += size;
            s->blocks++;
            off += 8;
        }
    }
    return n;
}

int main(int argc, char **argv) {
    static unsigned char work[WORK] __attribute__((aligned(8)));
    static struct subpool subs[256];
    int area = 1;
    bool hex = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-x") == 0) {
            hex = true;
        } else if (strcmp(argv[i], "-a") == 0 && i + 1 < argc) {
            const char *a = argv[++i];
            if (strcmp(a, "csa") == 0) area = 1;
            else if (strcmp(a, "sqa") == 0) area = 2;
            else if (strcmp(a, "lsqa") == 0) area = 3;
            else if (strcmp(a, "pvt") == 0) area = 4;
            else { fprintf(stderr, "ztop: no such area '%s'\n", a); return 2; }
        } else {
            fprintf(stderr, "usage: ztop [-a csa|sqa|lsqa|pvt] [-x]\n");
            return 2;
        }
    }

    struct zreq req = {.area = area, .len = WORK, .wk = work, .rc = -1};
    if (zvslist(&req) != 0) {
        fprintf(stderr, "ztop: the assembler stub does not know area %d\n", area);
        return 2;
    }
    if (req.rc != 0) {
        fprintf(stderr, "ztop: VSMLIST returned %d\n", req.rc);
        return 1;
    }
    if (hex)
        dump(work, 512);

    int n = parse(work, WORK, subs, 256);
    printf("%-8s %10s %8s\n", "subpool", "bytes", "blocks");
    unsigned long long total = 0;
    for (int i = 0; i < n; i++) {
        printf("%-8u %10llu %8u\n", subs[i].id, subs[i].bytes, subs[i].blocks);
        total += subs[i].bytes;
    }
    printf("%-8s %10llu\n", "total", total);
    return n > 0 ? 0 : 1;
}
