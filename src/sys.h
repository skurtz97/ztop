/**
 * @file sys.h
 * @brief The system's name and its address spaces, from the CVT and ASVT.
 *
 * Both are documented programming interfaces: CVTASVT at CVT+0x22C, the
 * ASVT with ASVTMAXU at +0x204 and ASVTENTY at +0x210, and the ASCB
 * fields ASCBASID, ASCBJBNS.  Reading them needs only problem state, so
 * this is the jobs view without an authorized service.
 */
#ifndef ZTOP_SYS_H
#define ZTOP_SYS_H

#include <stdbool.h>
#include <stdint.h>

#define ZJOB_MAX 512

/** @brief One address space. */
struct zjob {
    unsigned int asid;
    uint32_t ascb;
    char name[9];
};

/** @brief What the system view shows. */
struct zsys {
    char sysname[9];
    char host[65];
    char os[64];
    char machine[32];
    unsigned int slots; /**< ASVT entries, used or free */
    int njobs;
    struct zjob job[ZJOB_MAX];
};

/**
 * @brief Fill @p z from the CVT and the ASVT.
 * @param z Where to write.
 * @return false when this is not z/OS, with @c z untouched.
 */
bool zsys_read(struct zsys *z);

#endif
