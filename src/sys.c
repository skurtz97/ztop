/**
 * @file sys.c
 * @brief System identity and address spaces.
 *
 * The only z/OS-specific reads are four documented control blocks: the
 * CVT at PSA+0x10, CVTSNAME at CVT+0x154, CVTASVT at CVT+0x22C, and the
 * ASVT/ASCB chain below it.  Everything else (host name, release) comes
 * from the C library, so the structure is the same on any host.
 */
#include "sys.h"

#include <sys/utsname.h>
#include <unistd.h>

#ifdef __MVS__
#include <stdint.h>
#include <string.h>

/** @brief Read a big-endian halfword from an absolute address. */
static uint32_t rd32(uintptr_t a) {
    uint32_t v;

    memcpy(&v, (const void *)a, sizeof v);
    return v;
}

/** @brief Read a big-endian halfword from an absolute address. */
static uint16_t rd16(uintptr_t a) {
    uint16_t v;

    memcpy(&v, (const void *)a, sizeof v);
    return v;
}

/** @brief Read one byte from an absolute address. */
static unsigned char rd8(uintptr_t a) {
    return *(const unsigned char *)a;
}

/** @brief Copy an EBCDIC field and trim its blanks.
 *
 * The program runs in EBCDIC mode, and the terminal converts on output,
 * so the bytes are kept as read; only the trailing blanks (X'40') go. */
static void ebcdic(char *dst, size_t dstlen, const void *src, size_t srclen) {
    size_t n;

    if (dstlen == 0)
        return;
    n = srclen < dstlen - 1 ? srclen : dstlen - 1;
    if (n > 0)
        memcpy(dst, src, n);
    dst[n] = '\0';
    while (n > 0 && (unsigned char)dst[n - 1] == 0x40)
        dst[--n] = '\0';
}
#endif

/** @brief One nibble of a packed-decimal field. */
static unsigned int nib(uint32_t v, unsigned int shift) {
    return (v >> shift) & 0xFu;
}

bool zipl_decode(uint32_t date, uint32_t hundredths, struct zipl *out) {
    static const unsigned int mdays[12] = {
        31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31,
    };
    unsigned int century = nib(date, 24);
    unsigned int yy = nib(date, 20) * 10 + nib(date, 16);
    unsigned int ddd = nib(date, 12) * 100 + nib(date, 8) * 10 + nib(date, 4);
    unsigned int year, leap, secs, month = 0, day;

    if (century > 1 || ddd == 0 || ddd > 366)
        return false;
    year = (century == 1 ? 2000u : 1900u) + yy;
    leap = (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0)) ? 1u : 0u;
    if (ddd > 365 + leap)
        return false;
    secs = hundredths / 100;
    if (hundredths % 100 > 99 || secs >= 24 * 3600)
        return false;

    day = ddd;
    for (month = 0; month < 12; month++) {
        unsigned int len = mdays[month] + (month == 1 ? leap : 0);

        if (day <= len)
            break;
        day -= len;
    }
    out->year = year;
    out->month = month + 1;
    out->day = day;
    out->hour = secs / 3600;
    out->minute = (secs / 60) % 60;
    out->second = secs % 60;
    return true;
}

bool zsys_read(struct zsys *z) {
#ifdef __MVS__
    uintptr_t cvt, asvt;
    uint32_t maxu, entry;
    unsigned int i;

    memset(z, 0, sizeof *z);
    cvt = rd32(0x10);
    asvt = rd32(cvt + 0x22C);
    if (cvt == 0 || asvt == 0)
        return false;

    ebcdic(z->sysname, sizeof z->sysname, (const void *)(cvt + 0x154), 8);
    maxu = rd32(asvt + 0x204);
    z->slots = maxu > 0x10000 ? 0x10000 : maxu;
    for (i = 0; i < z->slots && z->njobs < ZJOB_MAX; i++) {
        struct zjob *j = &z->job[z->njobs];
        uint32_t namep;

        entry = rd32(asvt + 0x210 + (uintptr_t)i * 4);
        if (entry & 0x80000000u)
            continue; /* ASID is free */
        j->ascb = entry;
        j->asid = rd16((uintptr_t)entry + 0x24);
        namep = rd32((uintptr_t)entry + 0xB0);
        if (namep == 0)
            continue;
        ebcdic(j->name, sizeof j->name, (const void *)(uintptr_t)namep, 8);
        if (j->name[0] != '\0')
            z->njobs++;
    }
    z->cvt = (uint32_t)cvt;
    z->smca = rd32(cvt + 0xC4);
    z->real_k = rd32(cvt + 0x358);
    if (z->smca != 0)
        z->ipl_ok = zipl_decode(rd32((uintptr_t)z->smca + 340),
                                rd32((uintptr_t)z->smca + 336), &z->ipl);
    return true;
#else
    (void)z;
    return false;
#endif
}

bool zpage_read(struct zpage *p) {
#ifdef __MVS__
    uintptr_t cvt = rd32(0x10);
    uintptr_t asmvt;

    memset(p, 0, sizeof *p);
    if (cvt == 0)
        return false;
    asmvt = rd32(cvt + 0x2C0);
    if (asmvt == 0)
        return false;
    p->asmvt = (uint32_t)asmvt;
    p->common_pds = (rd8(asmvt) & 0x80u) != 0;
    p->local_paging = (rd8(asmvt) & 0x20u) == 0;
    p->plpa_pds = (rd8(asmvt) & 0x08u) != 0;
    p->plpa_full = (rd8(asmvt) & 0x04u) != 0;
    p->common_full = (rd8(asmvt) & 0x02u) != 0;
    p->quick_start = (rd8(asmvt + 1) & 0x10u) != 0;
    p->warm_start = (rd8(asmvt + 1) & 0x08u) != 0;
    p->vio_journaling = (rd8(asmvt + 0xB8) & 0x10u) != 0;
    p->vio_ci = rd32(asmvt + 0x8C);
    return true;
#else
    (void)p;
    return false;
#endif
}
