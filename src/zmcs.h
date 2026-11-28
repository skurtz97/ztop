/**
 * @file zmcs.h
 * @brief Extended MCS console: the one service that needs supervisor
 *        state.
 *
 * MCSOPER and MCSOPMSG both require supervisor state (SA22-7614-..,
 * "MCSOPER - Environment": minimum authorization supervisor state and
 * any PSW key), and MCSOPMSG also wants access-register mode, so the
 * calls live in src/zmcs.s: it switches to
 * supervisor state, calls the macro, switches back, and hands the
 * caller a copy of the message data block (MDB, IEAVM105, 160 bytes
 * plus text).  Nothing else in ztop needs a privileged instruction.
 *
 * The alternative, an EMCS console opened through an authorized
 * caller, is what this is: the program must be marked APF
 * (`extattr +ap ./ztop`, Makefile target `auth`) and the user needs
 * READ on MVS.MCSOPER.consname.
 *
 * The stub builds the MCSOPER/MODESET control blocks in its own CSECT, so
 * it is a 31-bit module of its own: an assembler CSECT linked into an
 * XPLINK program object is read-only, and the macros store into those
 * blocks (measured on CBT, 2026-11-27).  See the header of src/zmcs.s.
 *
 * The offsets in zmcs_req are the stub's interface and are checked in
 * src/ztop.c.  Keep the two in step.
 */
#ifndef ZTOP_ZMCS_H
#define ZTOP_ZMCS_H

#include <stdint.h>

/** Operations for zmcs_req.op. */
enum zmcs_op {
    ZMCS_ACTIVATE = 0,   /**< open the console, fills consid */
    ZMCS_GETMSG = 1,     /**< one message into buf, length in len */
    ZMCS_DEACTIVATE = 2, /**< close the console */
};

/**
 * @brief The request block the stub reads and writes.
 *
 * All pointers are 31-bit: the program is built -m32, so every object
 * it owns is reachable from an AMODE 31 assembler routine.
 */
typedef struct zmcs_req {
    int op;                      /**< 0: one of enum zmcs_op */
    char *name;                  /**< 4: 8 byte console name, blank padded */
    unsigned *consid;            /**< 8: console id, out */
    char *buf;                   /**< 12: MDB copy destination */
    int *len;                    /**< 16: in capacity, out copied */
    int rc;                      /**< 20: service return code */
    int rsn;                     /**< 24: service reason code */
    unsigned char ecb[4];        /**< 28: MSGECB, posted on a message */
    unsigned alet;               /**< 32: MCSCSAA: ALET, from the service */
    unsigned long long save[18]; /**< 40: the stub's save area */
    void *csa;                   /**< 184: MCSCSA: status area, from it */
} zmcs_req;

/** Largest MDB we copy out of the console's data space. */
enum { ZMCS_MDB_MAX = 4096 };

#ifdef __MVS__
/* HLASM upcases the ESD name of the CSECT, so the assembler side is
 * exported as ZMCS whatever case the source is written in, and the C
 * side has to ask for that name.  The stub is not XPLINK, so the call
 * uses the OS linkage convention, exactly like src/zvslist.S. */
int zmcs(zmcs_req *req) __asm__("ZMCS");
#pragma linkage(zmcs, OS)
#else
int zmcs(zmcs_req *req);
#endif

#endif /* ZTOP_ZMCS_H */
