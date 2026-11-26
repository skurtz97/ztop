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
    return true;
#else
    (void)z;
    return false;
#endif
}
