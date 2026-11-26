/**
 * @file vsm.c
 * @brief Parse the VSMLIST data area.  No z/OS dependencies: the parser is
 *        exercised on Linux against synthetic streams in tests/test_vsm.c.
 */
#include "vsm.h"

#include <stdio.h>
#include <string.h>

struct rd {
    const unsigned char *p;
    unsigned len;
    unsigned off;
    bool bad;
};

static uint32_t be32(const unsigned char *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static uint32_t rd32(struct rd *r)
{
    if (r->bad || r->off > r->len || r->len - r->off < 4) {
        r->bad = true;
        return 0;
    }
    uint32_t v = be32(r->p + r->off);
    r->off += 4;
    return v;
}

static unsigned rd8(struct rd *r)
{
    if (r->bad || r->off >= r->len) {
        r->bad = true;
        return 0;
    }
    return r->p[r->off++];
}

static bool fail(struct vsm_info *v, const char *what, unsigned off)
{
    (void)snprintf(v->err, sizeof v->err, "%s at offset %u", what, off);
    return false;
}

/* Every block needs at least its 8-byte address/length pair.  A count larger
 * than the remaining bytes can never be honest, and it would make us loop
 * for a long time on a corrupt or truncated data area. */
static bool count_ok(const struct rd *r, uint32_t n)
{
    if (r->off > r->len)
        return false;
    return n <= (r->len - r->off) / 8;
}

void vsm_reset(struct vsm_info *v, unsigned area, unsigned space)
{
    memset(v, 0, sizeof *v);
    v->area = area;
    v->space = space;
    v->lo = 0xFFFFFFFFu;
}

bool vsm_work_data(const unsigned char *work, unsigned wlen,
                   const unsigned char **data, unsigned *len)
{
    uint32_t addr, dlen;
    uintptr_t base = (uintptr_t)work;

    if (wlen < 12)
        return false;
    addr = be32(work + 4);
    dlen = be32(work + 8);

    if (dlen == 0) {
        *data = NULL;
        *len = 0;
        return true;
    }
    if ((uint64_t)addr < (uint64_t)base ||
        (uint64_t)addr + dlen > (uint64_t)base + wlen)
        return false;
    *data = (const unsigned char *)(uintptr_t)addr;
    *len = dlen;
    return true;
}

static struct vsm_subpool *sub_find(struct vsm_info *v, unsigned id, uint32_t tcb)
{
    for (int i = 0; i < v->nsub; i++)
        if (v->sub[i].id == id && v->sub[i].tcb == tcb)
            return &v->sub[i];
    if (v->nsub == VSM_MAX_SUB) {
        v->truncated = true;
        return NULL;
    }
    struct vsm_subpool *s = &v->sub[v->nsub++];
    memset(s, 0, sizeof *s);
    s->id = id;
    s->tcb = tcb;
    s->lo = 0xFFFFFFFFu;
    return s;
}

/* One allocated block, plus its free descriptors when SPACE=FREE.
 * Collected into s when there is room, and into the area totals always. */
static bool parse_block(struct rd *r, struct vsm_info *v, struct vsm_subpool *s)
{
    unsigned off = r->off;
    uint32_t addr = rd32(r);
    uint32_t blen = rd32(r);
    uint64_t bfree = 0;

    if (r->bad)
        return fail(v, "allocated block past end of data", off);

    if (v->space == VSM_FREE) {
        uint32_t nf = rd32(r);
        if (r->bad)
            return fail(v, "free-descriptor count past end of data", r->off - 4);
        if (!count_ok(r, nf))
            return fail(v, "free-descriptor count larger than the data area", r->off - 4);
        for (uint32_t i = 0; i < nf; i++) {
            unsigned foff = r->off;
            (void)rd32(r);               /* free block address */
            uint32_t flen = rd32(r);
            if (r->bad)
                return fail(v, "free descriptor past end of data", foff);
            bfree += flen;
        }
    }
    if (bfree > blen)
        bfree = blen;                    /* never report negative use */

    uint64_t used = (uint64_t)blen - bfree;
    v->blocks++;
    v->foot += blen;
    v->free += bfree;
    v->used += used;
    if (blen > v->largest)
        v->largest = blen;
    if (addr < v->lo)
        v->lo = addr;
    if ((uint64_t)addr + blen > v->hi)
        v->hi = (uint32_t)((uint64_t)addr + blen);

    if (s != NULL) {
        s->blocks++;
        s->foot += blen;
        s->free += bfree;
        s->used += used;
        if (blen > s->largest)
            s->largest = blen;
        if (addr < s->lo)
            s->lo = addr;
        if ((uint64_t)addr + blen > s->hi)
            s->hi = (uint32_t)((uint64_t)addr + blen);
    }
    return true;
}

static bool parse_alloc_section(struct rd *r, struct vsm_info *v)
{
    uint32_t nsub = rd32(r);
    if (r->bad)
        return fail(v, "subpool count past end of data", r->off - 4);
    /* A descriptor is at least 8 bytes, so this bound is safe. */
    if (nsub > (r->len - r->off) / 8 + 1)
        return fail(v, "subpool count larger than the data area", r->off - 4);

    for (uint32_t i = 0; i < nsub; i++) {
        unsigned off = r->off;
        unsigned fmt = rd8(r);
        unsigned dlen = rd8(r);
        unsigned id = rd8(r);
        unsigned flags = rd8(r);
        uint32_t tcb = rd32(r);

        if (r->bad)
            return fail(v, "subpool descriptor past end of data", off);
        if (fmt != 0)
            return fail(v, "subpool descriptor format byte is not zero", off);
        if (dlen < 8 || dlen > 64)
            return fail(v, "subpool descriptor length is out of range", off);
        if ((uint64_t)off + dlen > r->len)
            return fail(v, "subpool descriptor length past end of data", off);
        r->off = off + dlen;             /* skip trailing reserved bytes */

        uint32_t nblocks = rd32(r);
        if (r->bad)
            return fail(v, "allocated-block count past end of data", r->off - 4);
        if (!count_ok(r, nblocks))
            return fail(v, "allocated-block count larger than the data area", r->off - 4);

        struct vsm_subpool *s = sub_find(v, id,
                                         v->area == VSM_PVT ? tcb : 0);
        if (s != NULL) {
            s->key = flags & 0x0Fu;
            s->flags = flags;
        }
        for (uint32_t b = 0; b < nblocks; b++)
            if (!parse_block(r, v, s))
                return false;
    }
    return true;
}

bool vsm_parse_alloc(struct vsm_info *v, const unsigned char *data, unsigned len)
{
    struct rd r = { data, len, 0, false };

    if (v->area == VSM_PVT) {
        uint32_t n = rd32(&r);
        if (r.bad)
            return fail(v, "TCB count past end of data", r.off - 4);
        if (n > (r.len - r.off) / 4 + 1)
            return fail(v, "TCB count larger than the data area", r.off - 4);
        v->tcbs += n;
        for (uint32_t t = 0; t < n; t++) {
            unsigned off = r.off;
            (void)rd32(&r);              /* TCB address, for progress only */
            if (r.bad)
                return fail(v, "TCB address past end of data", off);
            if (!parse_alloc_section(&r, v))
                return false;
        }
    } else if (!parse_alloc_section(&r, v)) {
        return false;
    }
    return true;
}

bool vsm_parse_unalloc(struct vsm_info *v, const unsigned char *data, unsigned len)
{
    struct rd r = { data, len, 0, false };
    uint32_t nreg = rd32(&r);

    if (r.bad)
        return fail(v, "region count past end of data", r.off - 4);
    if (nreg > (r.len - r.off) / 12 + 1)
        return fail(v, "region count larger than the data area", r.off - 4);

    for (uint32_t i = 0; i < nreg; i++) {
        unsigned off = r.off;
        uint32_t addr = rd32(&r);
        uint32_t rlen = rd32(&r);

        if (r.bad)
            return fail(v, "region descriptor past end of data", off);
        if (v->nregion < VSM_MAX_REGION) {
            v->region[v->nregion].addr = addr;
            v->region[v->nregion].len = rlen;
        }
        v->nregion++;

        uint32_t nblocks = rd32(&r);
        if (r.bad)
            return fail(v, "unallocated-block count past end of data", r.off - 4);
        if (!count_ok(&r, nblocks))
            return fail(v, "unallocated-block count larger than the data area", r.off - 4);
        for (uint32_t b = 0; b < nblocks; b++) {
            unsigned boff = r.off;
            (void)rd32(&r);              /* unallocated block address */
            uint32_t blen = rd32(&r);
            if (r.bad)
                return fail(v, "unallocated block past end of data", boff);
            v->unalloc += blen;
            v->unalloc_blocks++;
            if (blen > v->largest_unalloc)
                v->largest_unalloc = blen;
        }
    }
    return true;
}
