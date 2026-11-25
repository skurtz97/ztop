* zvslist.s -- ask VSM what is allocated in one common area.
*
* VSMLIST has no C entry point and is a macro, so this is the only part
* of the project that has to be assembler.  Called from C as
* zvslist(&req) with plain OS linkage: R1 points at a one-word list
* holding the address of the request block.
*
*   struct zreq { int area; unsigned len; void *wk; int rc;
*                 void *self; unsigned long long save[18]; }
*
* area 1 CSA, 2 SQA, 3 LSQA, 4 private; len is the work area length;
* rc receives the service's return code (0 complete, 4 "did not fit,
* ask again", anything else is an error).
*
* Returns 0 if the service was called, -1 for an unknown area.
*
* The save area lives in the caller's block (offset 24) rather than in
* this CSECT: in an XPLINK load module an assembler CSECT is read-only,
* and the first store into it takes a 0C4.  Only the base register and
* the literal pool stay here, and both are read-only.
*
* SPACE=ALLOC and LOC=31 are the macro defaults.  LINKAGE=SVC is
* used rather than the default SYSTEM: the PC-number path through
* CVT+772/+88 branches off to a bad address here, the branch entry
* does not.
*
* VSMLIST writes its control block into the first 32 bytes of the work
* area and needs the register operand in doubled parentheses: with
* WKAREA=(4,5) the macro generates "LA 1,4" and stores at address 12.
*
* The offsets below are shared with src/ztop.c and checked there.
         SYSSTATE AMODE64=NO,ARCHLVL=2,OSREL=ZOSV1R6
zvslist  CSECT
zvslist  AMODE 31
zvslist  RMODE 24
         STM   14,12,12(13)
         BALR  12,0
         USING *,12
         L     12,=A(zvslist)         the macro needs a literal pool
         L     2,0(1)                 R2 -> struct zreq
         ST    2,16(,2)              self: the macro clobbers registers
         LA    10,24(,2)             our save area, in that block
         ST    13,4(,10)             chain it to the caller's
         ST    10,8(,13)
         LR    13,10
         L     3,0(2)                 area
         L     5,4(2)                 work area length
         L     4,8(2)                 work area address
         XC    0(4,4),0(4)            return code zero before the call
         LHI   15,-1
         ST    15,12(2)              unknown area until proven
         CHI   3,1                   the area codes are small
         BE    CSA
         CHI   3,2
         BE    SQA
         CHI   3,3
         BE    LSQA
         CHI   3,4
         BE    PVT
         B     OUT
CSA      VSMLIST SP=CSA,WKAREA=((4),5)
         B     DONE
SQA      VSMLIST SP=SQA,WKAREA=((4),5)
         B     DONE
LSQA     VSMLIST SP=LSQA,WKAREA=((4),5)
         B     DONE
PVT      VSMLIST SP=PVT,WKAREA=((4),5)
DONE     L     2,16(,13)              self, from our save area's block
         L     4,8(2)                 work area
         L     15,0(4)               service return code
         ST    15,12(2)
         SR    15,15
OUT      L     13,4(,13)              back to the caller's save area
         LM    14,12,12(13)
         BR    14
         LTORG
         END
