*
* zmcs -- the extended MCS console.  MCSOPER and MCSOPMSG both need
* supervisor state (SA22-7614 "Environment": supervisor state, any PSW
* key) and MCSOPMSG also wants access-register mode.
*
* The caller fills a zmcs_req (src/zmcs.h) and calls zmcs() with it.
* The offsets below are that block, -m32, asserted by the C side.
*
* Module rule: MODESET and MCSOPER build their control blocks here and
* store into them at run time, so this has to be a 31-bit module of its
* own, reached through a thunk.  An assembler CSECT linked into an
* XPLINK program object is read-only and that store is a 0C4.
*
zmcs     CSECT
zmcs     AMODE 31
zmcs     RMODE 24
*
* Request block offsets (src/zmcs.h).
ZR_OP    EQU   0                   int op
ZR_NAME  EQU   4                   char *name
ZR_CONS  EQU   8                   unsigned *consid, out
ZR_BUF   EQU   12                  char *buf
ZR_LEN   EQU   16                  int *len, in capacity, out count
ZR_RC    EQU   20                  int rc
ZR_RSN   EQU   24                  int rsn
ZR_ECB   EQU   28                  unsigned char ecb[4], MSGECB
ZR_ALET  EQU   32                  unsigned alet, out, MCSCSAA
ZR_SAVE  EQU   40                  save area, 18 fullwords
ZR_CSA   EQU   184                 void *csa, out, MCSCSA
*
* Operations (enum zmcs_op).
ZOP_ACT  EQU   0
ZOP_GET  EQU   1
ZOP_DEA  EQU   2
*
         STM   R14,R12,12(R13)
         BALR  R12,R0
         USING *,R12
         LAE   R12,R0(R0,R12)      AR12 = 0: our own storage
         L     R2,0(R1)            R1 -> list, word 0 = the request
         LTR   R2,R2
         BZ    BADREQ
         LAE   R11,R0(R0,R2)       AR11 = 0 too
         LA    R10,ZR_SAVE(,R11)   save area, inside the request
         ST    R13,4(,R10)         the caller's, for a dump
         ST    R10,8(,R13)
         LR    R13,R10
         L     R3,ZR_OP(,R11)
         CHI   R3,ZOP_ACT
         BE    DOACT
         CHI   R3,ZOP_GET
         BE    DOGET
         CHI   R3,ZOP_DEA
         BE    DODEACT
         LA    R3,4               unknown request
         B     BACK
*
* ACTIVATE, in primary space mode: MCSOPER does not need AR mode.
* MCSCSA and MCSCSAA are output areas: the service writes the address
* of the console status area and its ALET there.  QLIMIT bounds the
* queue, so a console left open cannot grow until storage runs out.
*
DOACT    DS    0H
         MODESET MODE=SUP
         LTR   R15,R15
         BNZ   NORIGHTS
         L     R5,ZR_NAME(,R11)   NAME and TERMNAME
         L     R6,ZR_CONS(,R11)   CONSID, out
         L     R4,ZR_CSA(,R11)    MCSCSA, out
         L     R3,ZR_ALET(,R11)   MCSCSAA, out
         LHI   R9,ZMCS_QLIMIT
         MCSOPER REQUEST=ACTIVATE,                                     X
               NAME=(R5),                                              X
               TERMNAME=(R5),                                          X
               MSGDLVRY=FIFO,                                          X
               MSGECB=ZR_ECB(,R11),                                    X
               MCSCSA=(R4),                                            X
               MCSCSAA=(R3),                                           X
               CONSID=(R6),                                            X
               QLIMIT=(R9),                                            X
               RTNCODE=ZR_RC(,R11),                                    X
               RSNCODE=ZR_RSN(,R11)
         MODESET MODE=PROB
         B     BACKRC
*
* GETMSG.  The service wants AR mode and hands back the MDB address in
* R1 with its ALET in AR1.  The MDB lies in the console's data space,
* so it is copied out here, still in AR mode.  RC 8 is "nothing
* queued", the steady state.
*
DOGET    DS    0H
         SAC   0                   primary mode for MODESET
         MODESET MODE=SUP
         LTR   R15,R15
         BNZ   NORIGHTS
         SAC   512                 AR mode for MCSOPMSG
         SYSSTATE ASCENV=AR
         L     R6,ZR_CONS(,R11)
         MCSOPMSG REQUEST=GETMSG,                                      X
               CONSID=(R6),                                            X
               RTNCODE=ZR_RC(,R11),                                    X
               RSNCODE=ZR_RSN(,R11)
         CHI   R15,0
         BE    DOCOPY
         CHI   R15,ZMCS_RC_QUEUED
         BNE   GETEND
DOCOPY   DS    0H
         LAE   R8,0(R0,R1)         R8 = MDB, AR8 = the MDB's ALET
         LH    R4,0(,R8)          MDBLEN, the first halfword
         L     R5,ZR_LEN(,R11)    capacity the caller offered
         CR    R4,R5
         BNH   COPYOK
         LR    R4,R5              clip
COPYOK   DS    0H
         ST    R4,ZR_LEN(,R11)    length copied
         L     R3,ZR_BUF(,R11)
         LAE   R3,0(R0,R3)        AR3 = 0: our own space
         LTR   R4,R4
         BZ    GETEND
CLOOP    DS    0H
         MVC   0(1,R3),0(R8)
         LA    R3,1(,R3)
         LA    R8,1(,R8)
         BCT   R4,CLOOP
GETEND   DS    0H
         SAC   0                   primary mode for MODESET
         SYSSTATE ASCENV=P
         MODESET MODE=PROB
         B     BACKRC
*
* DEACTIVATE: hand the console id back.
*
DODEACT  DS    0H
         MODESET MODE=SUP
         LTR   R15,R15
         BNZ   NORIGHTS
         L     R6,ZR_CONS(,R11)
         MCSOPER REQUEST=DEACTIVATE,                                   X
               CONSID=(R6),                                            X
               RTNCODE=ZR_RC(,R11),                                    X
               RSNCODE=ZR_RSN(,R11)
         MODESET MODE=PROB
         B     BACKRC
*
* Returns.  NORIGHTS is the MODESET refusal: an unauthorized program
* never gets here, the restricted SVC ends the task with S047.
*
NORIGHTS DS    0H
         ST    R15,ZR_RSN(,R11)   the MODESET reason
         LA    R3,12
         ST    R3,ZR_RC(,R11)
         B     BACK
BADREQ   DS    0H
         LA    R3,4
         B     BACK
BACKRC   DS    0H
         L     R3,ZR_RC(,R11)
BACK     DS    0H
         LR    R2,R11
         L     R13,4(,R13)
         LM    R14,R12,12(R13)
         BR    R14
*
* Constants.
ZMCS_QLIMIT    EQU 2000            messages the console may queue
ZMCS_RC_QUEUED EQU 4               message returned, queueing suspended
*
* Registers, by name, so a number is never read as a register.
R0       EQU   0
R1       EQU   1
R2       EQU   2
R3       EQU   3
R4       EQU   4
R5       EQU   5
R6       EQU   6
R7       EQU   7
R8       EQU   8
R9       EQU   9
R10      EQU   10
R11      EQU   11
R12      EQU   12
R13      EQU   13
R14      EQU   14
R15      EQU   15
         END
