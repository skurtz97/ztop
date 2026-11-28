/** @file uss.c
 *  @brief z/OS UNIX readers: mounted file systems and processes.
 *
 *  w_getmntent() fills one contiguous header+entries buffer; statvfs() is
 *  then asked for the size and the read-only flag, so no mount-mode bit has
 *  to be decoded.  w_getpsent() is a walk: token 0 is the first process and
 *  each call returns the token of the next one.
 */
#ifdef __MVS__
#define _OPEN_SYS 1     /* sys/mntent.h, sys/ps.h */
#define _OPEN_SYS_EXT 1 /* __errno2() */
#endif

#include "uss.h"

#ifdef __MVS__
#include <errno.h>
#include <sys/mntent.h>
#include <sys/ps.h>
#include <sys/statvfs.h>
#include <time.h> /* CLOCKS_PER_SEC */
#endif

#include <stdio.h>
#include <string.h>

#ifdef __MVS__
/** Copy at most @p n bytes of a name into @p dst, trimmed of blanks. */
static void name_copy(char *dst, size_t len, const char *src, size_t n) {
    size_t i = 0;

    while (i < n && src[i] != '\0' && src[i] != ' ')
        i++;
    if (i >= len)
        i = len - 1;
    memcpy(dst, src, i);
    dst[i] = '\0';
}

/** One w_getmntent() buffer: header, then the entries the service fills. */
static struct {
    MNTE3H h;
    W_MNTENT3 e[USS_MAX_FS];
} mnt;

bool uss_fs_read(uss_fs_list *out) {
    int n, i, fit;

    memset(out, 0, sizeof *out);
    memset(&mnt, 0, sizeof mnt);
    memcpy(mnt.h.mnt3H_cbid, MNTE3H_ID, 4);
    mnt.h.mnt3H_cblen = (int)sizeof mnt;
    mnt.h.mnt3H_bodylen = (int)sizeof(W_MNTENT3);

    n = w_getmntent((char *)&mnt, (int)sizeof mnt);
    if (n < 0) {
        (void)snprintf(out->err, sizeof out->err, "w_getmntent: errno %d",
                       errno);
        return false;
    }
    /* The header length is the buffer size on the way in and what the
     * service wrote on the way out; either can be smaller than the
     * entries it reports, and a short one must not become a negative
     * row count. */
    fit = (int)((mnt.h.mnt3H_cblen - (int)sizeof(MNTE3H)) /
                (int)sizeof(W_MNTENT3));
    if (n == 0) /* some levels report the count only here */
        n = fit;
    if (fit < n)
        n = fit;
    if (n < 0)
        n = 0;
    out->truncated = n > USS_MAX_FS;
    out->nfs = n > USS_MAX_FS ? USS_MAX_FS : n;

    for (i = 0; i < out->nfs; i++) {
        const W_MNTENT3 *e = &mnt.e[i];
        uss_fs *f = &out->fs[i];
        struct statvfs v;

        name_copy(f->fsname, sizeof f->fsname, e->mnt3_fsname,
                  sizeof f->fsname);
        name_copy(f->type, sizeof f->type, e->mnt3_fstname, sizeof f->type);
        name_copy(f->mount, sizeof f->mount - 1, e->mnt3_mountpoint,
                  sizeof e->mnt3_mountpoint);
        f->pct = -1;
        if (statvfs(e->mnt3_mountpoint, &v) != 0) {
            f->eno = errno;
            continue;
        }
        f->ok = true;
        f->rdonly = (v.f_flag & ST_RDONLY) != 0;
        {
            unsigned long unit = v.f_frsize ? v.f_frsize : v.f_bsize;
            unsigned long long total = (unsigned long long)v.f_blocks * unit;
            unsigned long long avail = (unsigned long long)v.f_bavail * unit;

            f->kb_total = total / 1024;
            f->kb_free = avail / 1024;
            f->pct = total ? (int)((total - avail) * 100 / total) : -1;
        }
    }
    return true;
}

bool uss_ps_read(uss_ps_list *out) {
    int token = 0;

    memset(out, 0, sizeof *out);
    for (;;) {
        W_PSPROC ps;
        char tty[32], path[128], cmd[128];
        int next;

        if (out->nproc >= USS_MAX_PS) {
            out->truncated = true;
            break;
        }
        memset(&ps, 0, sizeof ps);
        memset(tty, 0, sizeof tty);
        memset(path, 0, sizeof path);
        memset(cmd, 0, sizeof cmd);
        ps.ps_conttylen = (int)sizeof tty;
        ps.ps_conttyptr = tty;
        ps.ps_pathlen = (int)sizeof path;
        ps.ps_pathptr = path;
        ps.ps_cmdlen = (int)sizeof cmd;
        ps.ps_cmdptr = cmd;

        next = w_getpsent(token, &ps, sizeof ps);
        if (next == 0) /* no more processes this caller may see */
            break;
        if (next < 0) {
            (void)snprintf(out->err, sizeof out->err, "w_getpsent: errno %d",
                           errno);
            return out->nproc > 0;
        }
        token = next;

        {
            uss_proc *p = &out->proc[out->nproc];

            p->pid = (int)ps.ps_pid;
            p->ppid = (int)ps.ps_ppid;
            p->uid = (int)ps.ps_euid; /* effective UID */
            p->state = ps.ps_state;
            p->st = (char)(ps.ps_state & PS_STATECHAR);
            p->swapped = (ps.ps_state & PS_SWAP) != 0;
            p->cpu_ms = (unsigned long)((ps.ps_usertime + ps.ps_systime) *
                                        1000 / CLOCKS_PER_SEC);
            name_copy(p->tty, sizeof p->tty, tty, sizeof tty);
            name_copy(p->cmd, sizeof p->cmd, cmd, sizeof cmd);
        }
        out->nproc++;
    }
    return true;
}
#else  /* !__MVS__ */
bool uss_fs_read(uss_fs_list *out) {
    memset(out, 0, sizeof *out);
    (void)snprintf(out->err, sizeof out->err,
                   "mounted file systems are only available on z/OS");
    return false;
}

bool uss_ps_read(uss_ps_list *out) {
    memset(out, 0, sizeof *out);
    (void)snprintf(out->err, sizeof out->err,
                   "process data is only available on z/OS");
    return false;
}
#endif /* __MVS__ */
