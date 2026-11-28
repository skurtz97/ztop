/* Synthetic VSMLIST data areas, built from the layouts in the z/OS
 * Authorized Assembler Services Guide.  These tests do not need z/OS. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../src/vsm.h"

static unsigned char buf[8192];
static unsigned len;

static void put8(unsigned v) {
    if (len < sizeof buf)
        buf[len++] = (unsigned char)v;
}

static void put32(uint32_t v) {
    put8(v >> 24);
    put8(v >> 16);
    put8(v >> 8);
    put8(v);
}

static void desc(unsigned id, unsigned flags, uint32_t tcb, unsigned dlen) {
    put8(0);
    put8(dlen);
    put8(id);
    put8(flags);
    put32(tcb);
    for (unsigned i = 8; i < dlen; i++)
        put8(0);
}

static void block(uint32_t addr, uint32_t blen) {
    put32(addr);
    put32(blen);
}

static int fail_line(int line, const char *what) {
    (void)fprintf(stderr, "test_vsm.c:%d: %s\n", line, what);
    return 1;
}

#define CHECK(c)                                                               \
    do {                                                                       \
        if (!(c))                                                              \
            return fail_line(__LINE__, #c);                                    \
    } while (0)

static int test_csa(void) {
    vsm_info v;

    len = 0;
    put32(2);
    desc(227, 0x08, 0, 8);
    put32(2);
    block(0x1000, 0x1000);
    put32(2);
    block(0x1000, 0x100);
    block(0x1200, 0x200);
    block(0x3000, 0x800);
    put32(0);
    desc(228, 0x09, 0, 8);
    put32(1);
    block(0x4000, 0x400);
    put32(1);
    block(0x4000, 0x400);

    vsm_reset(&v, VSM_CSA, VSM_FREE);
    CHECK(vsm_parse_alloc(&v, buf, len));
    CHECK(v.nsub == 2);
    CHECK(v.blocks == 3);
    CHECK(v.foot == 0x1C00);
    CHECK(v.free == 0x700);
    CHECK(v.used == 0x1500);
    CHECK(v.largest == 0x1000);
    CHECK(v.sub[0].id == 227);
    CHECK(v.sub[0].key == 8);
    CHECK(v.sub[0].used == 0x1500);
    CHECK(v.sub[0].foot == 0x1800);
    CHECK(v.sub[0].blocks == 2);
    CHECK(v.sub[0].lo == 0x1000);
    CHECK(v.sub[0].hi == 0x3800);
    CHECK(v.sub[1].id == 228);
    CHECK(v.sub[1].key == 9);
    CHECK(v.sub[1].used == 0);
    CHECK(v.sub[1].free == 0x400);
    return 0;
}

static int test_merge(void) {
    vsm_info v;

    len = 0;
    put32(1);
    desc(227, 0x08, 0, 8);
    put32(1);
    block(0x1000, 0x1000);
    put32(0);

    vsm_reset(&v, VSM_CSA, VSM_FREE);
    CHECK(vsm_parse_alloc(&v, buf, len));
    CHECK(vsm_parse_alloc(&v, buf, len));
    CHECK(v.nsub == 1);
    CHECK(v.blocks == 2);
    CHECK(v.used == 0x2000);
    CHECK(v.sub[0].blocks == 2);
    return 0;
}

static int test_line16(void) {
    vsm_info v;

    len = 0;
    put32(1);
    desc(230, 0x08, 0, 8);
    put32(2);
    block(0x00FF0000, 0x1000); /* ends exactly at the line: below */
    put32(0);
    block(0x01000000, 0x1000); /* starts at the line: above */
    put32(0);

    vsm_reset(&v, VSM_CSA, VSM_FREE);
    CHECK(vsm_parse_alloc(&v, buf, len));
    CHECK(v.used == 0x2000);
    CHECK(v.below == 0x1000);
    CHECK(v.above == 0x1000);
    CHECK(v.sub[0].below == 0x1000);
    CHECK(v.sub[0].above == 0x1000);
    return 0;
}

static int test_pvt(void) {
    vsm_info v;

    len = 0;
    put32(1);
    put32(0x00F00000);
    put32(1);
    desc(0, 0x08, 0x00F00000, 8);
    put32(1);
    block(0x100000, 0x2000);
    put32(0);

    vsm_reset(&v, VSM_PVT, VSM_FREE);
    CHECK(vsm_parse_alloc(&v, buf, len));
    CHECK(v.tcbs == 1);
    CHECK(v.nsub == 1);
    CHECK(v.sub[0].id == 0);
    CHECK(v.sub[0].tcb == 0x00F00000);
    CHECK(v.used == 0x2000);
    return 0;
}

static int test_unalloc(void) {
    vsm_info v;

    len = 0;
    put32(2);
    put32(0x00A00000);
    put32(0x00100000);
    put32(2);
    block(0x00A00100, 0x100);
    block(0x00A80000, 0x40000);
    put32(0x00B00000);
    put32(0x00200000);
    put32(1);
    block(0x00B10000, 0x8000);

    vsm_reset(&v, VSM_CSA, VSM_UNALLOC);
    CHECK(vsm_parse_unalloc(&v, buf, len));
    CHECK(v.unalloc_blocks == 3);
    CHECK(v.unalloc == 0x48100);
    CHECK(v.largest_unalloc == 0x40000);
    CHECK(v.nregion == 2);
    CHECK(v.region[0].addr == 0x00A00000);
    CHECK(v.region[1].len == 0x00200000);
    return 0;
}

static int test_bad(void) {
    vsm_info v;

    /* descriptor length below the documented minimum */
    len = 0;
    put32(1);
    desc(227, 0, 0, 7);
    vsm_reset(&v, VSM_CSA, VSM_FREE);
    CHECK(!vsm_parse_alloc(&v, buf, len));

    /* format byte is not zero */
    len = 0;
    put32(1);
    put8(1);
    put8(8);
    put8(227);
    put8(0);
    put32(0);
    put32(0);
    vsm_reset(&v, VSM_CSA, VSM_FREE);
    CHECK(!vsm_parse_alloc(&v, buf, len));

    /* an impossible block count must not spin */
    len = 0;
    put32(1);
    desc(227, 0, 0, 8);
    put32(0x10000000u);
    vsm_reset(&v, VSM_CSA, VSM_FREE);
    CHECK(!vsm_parse_alloc(&v, buf, len));

    /* truncated block */
    len = 0;
    put32(1);
    desc(227, 0, 0, 8);
    put32(1);
    put8(0);
    vsm_reset(&v, VSM_CSA, VSM_FREE);
    CHECK(!vsm_parse_alloc(&v, buf, len));

    /* truncated unallocated block */
    len = 0;
    put32(1);
    put32(0x1000);
    put32(0x1000);
    put32(1);
    put8(0);
    vsm_reset(&v, VSM_CSA, VSM_UNALLOC);
    CHECK(!vsm_parse_unalloc(&v, buf, len));

    return 0;
}

int main(void) {
    int rc = 0;

    rc |= test_csa();
    rc |= test_merge();
    rc |= test_line16();
    rc |= test_pvt();
    rc |= test_unalloc();
    rc |= test_bad();
    if (rc == 0)
        (void)printf("test_vsm: ok\n");
    return rc;
}
