* zvslist.s -- ask VSM what is allocated in one of the common areas.
*
* VSMLIST has no C equivalent and is a macro, so this is the one place the
* project has to be assembler.  One entry, zvslist(), called from C with
* OS_UPSTACK linkage: R1 points to a one-word parameter list holding the
* address of the request block.
*
*   struct zreq { int area; unsigned len; void *wk; int rc; }
*
* area: 1 CSA, 2 SQA, 3 LSQA, 4 private.  len is the length of the work
* area in bytes; the caller zeroes nothing, this module zeroes the four
* return code bytes VSMLIST needs zero before the first call.
*
* Returns 0 if the call was issued, or -1 for an unknown area.  The
* service's own return code is left in zreq.rc: 0 complete, 4 "did not
* fit, ask again", other values are errors (see the Assembler Services
* Reference, "Return Codes for the VSMLIST Macro").
*
* Offsets of the area codes and of the request fields are shared with
* src/ztop.c; keep the two in step.
         SYSSTATE AMODE64=NO,ARCHLVL=2,OSREL=ZOSV1R6
ZTSTK    CSECT
ZTSTK    AMODE 31
ZTSTK    RMODE 24
         STM   14,12,12(13)          standard OS entry: caller's F5SA
         LR    11,15
         USING ZTSTK,11
         L     2,0(1)                R2 -> struct zreq
         L     3,0(2)                area
         L     5,4(2)                work area length
         L     4,8(2)                work area address
         XC    0(4,4),0(4)           VSMLIST wants the return code zeroed
         ST    5,12(2)              assume failure until the service says
         C     3,=F'1'
         BE    CSA
         C     3,=F'2'
         BE    SQA
         C     3,=F'3'
         BE    LSQA
         C     3,=F'4'
         BE    PVT
         LHI   15,-1
         B     OUT
CSA      VSMLIST SP=CSA,WKAREA=(4,5),SPACE=ALLOC,LOC=31,LINKAGE=SYSTEM
         B     DONE
SQA      VSMLIST SP=SQA,WKAREA=(4,5),SPACE=ALLOC,LOC=31,LINKAGE=SYSTEM
         B     DONE
LSQA     VSMLIST SP=LSQA,WKAREA=(4,5),SPACE=ALLOC,LOC=31,LINKAGE=SYSTEM
         B     DONE
PVT      VSMLIST SP=PVT,WKAREA=(4,5),SPACE=ALLOC,LOC=31,LINKAGE=SYSTEM
DONE     L     4,8(2)                work area again, R4 is not preserved
         L     15,0(4)              service return code
         ST    15,12(2)
         SR    15,15
OUT      LM    14,12,12(13)
         BR    14
         LTORG
         END
