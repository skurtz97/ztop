/** @file uss.h
 *  @brief z/OS UNIX readers: processes and mounted file systems.
 *
 *  Two fixed-capacity tables filled in place.  Neither reader allocates, and
 *  on a host that is not z/OS both report the same message the CVT readers
 *  print, so the portable build still links and the view stays honest.
 */
#ifndef ZTOP_USS_H
#define ZTOP_USS_H

#include <stdint.h>

enum {
    USS_MAX_FS = 64,  /**< mounted file systems kept */
    USS_MAX_PS = 128, /**< processes kept */
};

/** @brief One mounted file system, from w_getmntent() and statvfs(). */
typedef struct uss_fs {
    char fsname[45]; /**< aggregate data set name */
    char mount[128]; /**< mount point, trimmed */
    char type[9];    /**< file system type name */
    bool rdonly;     /**< statvfs says ST_RDONLY */
    bool ok;         /**< statvfs succeeded */
    uint64_t kb_total;
    uint64_t kb_free;
    int pct; /**< 0..100, or -1 when the size is unknown */
    int eno; /**< errno from a failed statvfs() */
} uss_fs;

typedef struct uss_fs_list {
    uss_fs fs[USS_MAX_FS];
    int nfs;
    bool truncated;
    char err[160];
} uss_fs_list;

/** @brief One process, from w_getpsent(). */
typedef struct uss_proc {
    int pid;
    int ppid;
    int uid;
    unsigned state; /**< raw ps_state: letter in low bits, flags above */
    char st;        /**< the state letter, ps_state & PS_STATECHAR */
    bool swapped;
    unsigned long cpu_ms; /**< user + system time in milliseconds */
    char tty[16];
    char cmd[64];
} uss_proc;

typedef struct uss_ps_list {
    uss_proc proc[USS_MAX_PS];
    int nproc;
    bool truncated;
    char err[160];
} uss_ps_list;

bool uss_fs_read(uss_fs_list *out);
bool uss_ps_read(uss_ps_list *out);

#endif
