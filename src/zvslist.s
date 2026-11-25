* zvslist.s -- ask VSM what is allocated in one common area.
*
* VSMLIST has no C entry point and is a macro, so this is the only part
* of the project that has to be assembler.  Called from C as
* zvslist(&req) with OS_UPSTACK linkage: R1 points at a one-word
* parameter list holding the address of the request block.
*
*   struct zreq { int area; unsigned len; void *wk; int rc; }
*
* area 1 CSA, 2 SQA, 3 LSQA, 4 private; len is the work area length;
* rc receives the service's return code (0 complete, 4 "did not fit,
* ask again", anything else is an error).
*
* Returns 0 if the service was called, -1 for an unknown area.
*
* The request offsets and the area codes are shared with src/ztop.c.
         SYSSTATE AMODE64=NO,ARCHLVL=2,OSREL=ZOSV1R6
ZTSTK    CSECT
ZTSTK    AMODE 31
ZTSTK    RMODE 24
         STM   14,12,12(13)
         LR    12,15
         USING ZTSTK,12
         LA    14,SAVE
         ST    13,4(,14)
         ST    14,8(,13)
         LR    13,14
         L     2,0(1)                 R2 -> struct zreq
         ST    2,REQ                  remember it, the macro clobbers
         L     3,0(2)                 area
         L     5,4(2)                 work area length
         L     4,8(2)                 work area address
         XC    0(4,4),0(4)            return code zero before the call
         LHI   15,-1
         ST    15,12(2)               unknown area until proven otherwise
         C     3,=F'1'
         BE    CSA
         C     3,=F'2'
         BE    SQA
         C     3,=F'3'
         BE    LSQA
         C     3,=F'4'
         BE    PVT
         B     OUT
CSA      VSMLIST SP=CSA,WKAREA=(4,5),SPACE=ALLOC,LOC=31,+
               LINKAGE=SYSTEM
         B     DONE
SQA      VSMLIST SP=SQA,WKAREA=(4,5),SPACE=ALLOC,LOC=31,+
               LINKAGE=SYSTEM
         B     DONE
LSQA     VSMLIST SP=LSQA,WKAREA=(4,5),SPACE=ALLOC,LOC=31,+
               LINKAGE=SYSTEM
         B     DONE
PVT      VSMLIST SP=PVT,WKAREA=(4,5),SPACE=ALLOC,LOC=31,+
               LINKAGE=SYSTEM
DONE     L     2,REQ                  reload: only R15 is promised
         L     4,8(2)                 work area
         L     15,0(4)               service return code
         ST    15,12(2)
         SR    15,15
OUT      L     13,4(,13)
         LM    14,12,12(13)
         BR    14
SAVE     DC    18F'0'
REQ      DS    A
         LTORG
         END
