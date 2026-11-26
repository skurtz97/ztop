/**
 * @file sys.h
 * @brief The system's identity, its IPL, its storage and its address spaces.
 *
 * Everything here is a documented programming interface reached from the
 * CVT at PSA+0x10: CVTSNAME, CVTSMCA, CVTRLSTG, CVTASMVT and CVTASVT, the
 * ASVT/ASCB chain, and the SMCA and ASMVT control blocks behind them.
 * Reading them needs only problem state, so none of this needs an
 * authorized service.
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

/** @brief The IPL date and time, decoded from the SMCA. */
struct zipl {
    unsigned int year;
    unsigned int month;
    unsigned int day; /**< day of month */
    unsigned int hour;
    unsigned int minute;
    unsigned int second;
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
    bool ipl_ok;
    struct zipl ipl;
    uint64_t real_k; /**< real storage online at IPL, in K */
    uint32_t cvt;
    uint32_t smca;
};

/**
 * @brief Decode SMCAIDTE (0CYYDDDF, packed) and SMCAITME (hundredths).
 * @param date SMCAIDTE as read.
 * @param hundredths SMCAITME as read.
 * @param out Where to write.
 * @return false when the fields do not describe a real date and time.
 */
bool zipl_decode(uint32_t date, uint32_t hundredths, struct zipl *out);

/**
 * @brief Fill @p z from the CVT, the SMCA and the ASVT.
 * @param z Where to write.
 * @return false when this is not z/OS, with @c z untouched.
 */
bool zsys_read(struct zsys *z);

/** @brief Auxiliary storage state, from the ASMVT. */
struct zpage {
    bool common_pds; /**< a common page data set exists */
    bool common_full;
    bool plpa_pds;
    bool plpa_full;
    bool local_paging; /**< local page data sets are in use */
    bool vio_journaling;
    bool quick_start;
    bool warm_start;
    unsigned int vio_ci; /**< control intervals in the VIO journal */
    uint32_t asmvt;
};

/**
 * @brief Fill @p p from the ASMVT.
 * @param p Where to write.
 * @return false when this is not z/OS or there is no ASMVT.
 */
bool zpage_read(struct zpage *p);

#endif
