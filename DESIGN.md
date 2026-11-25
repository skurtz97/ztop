# ztop design

Decisions, not a wish list.  Anything not yet measured says so.

## Where the numbers come from

`VSMLIST` (MVS Authorized Assembler Services Reference, chapter 30).  It is
the only documented interface that reports allocated storage by subpool for
CSA, SQA, LSQA and the private area.  Per subpool it returns the 4K-multiple
blocks that hold allocations and the bytes allocated inside them.

## Authorization: APF, by design

This is an administrator's utility, so it is APF-authorized and uses
`VSMLIST` with `LINKAGE=SYSTEM`, the path the reference opens to an
authorized program (problem state, PSW key 8-15, `LINKAGE=BRANCH` would
want supervisor state and key 0 and is not used).

Measured once without APF, on z/OS 3.1 (29.00), before the decision, and
kept here so nobody repeats it:

- `LINKAGE=SYSTEM`: `Possible Bad Branch` through the macro's PC-number path
  (`L 15,16(0,0)` CVT, `+772` SVT, `+88`, `PC 0(15)`).
- `LINKAGE=BRANCH`: 0C1 at the `CVT+1140` branch entry, which is what the
  reference promises for supervisor-state-only.
- `LINKAGE=SVC` (SVC 72): 0C4 inside the service.

What the installation needs, none of it in the tool:

- an APF-listed load path.  `D PROG,APF` on CBT lists 54 entries and every
  one is a partitioned data set -- no HFS directory is APF-authorized, so
  one has to be added (PROGxx `APF ADD DSNAME('/some/dir')`, or
  `SETPROG APF,ADD,DSNAME=...` at runtime) before an executable in the z/OS
  UNIX file system can be marked;
- `extattr +ap` on the executable that lives there;
- a review of the program itself, because everything it does now runs with
  the installation's authority.  ztop only reads VSM data and writes to
  standard output; it must not grow a feature that takes a path, a command
  or a name from an untrusted caller without stripping authority first.

## What ZOAU already provides

Checked so it is not duplicated: ZOAU 1.4 ships no storage or subpool
facility.  `zinfo` reports IPL, CPU, system and IODF information only;
`apfadm`, `mvscmd`, `mvscmdauth`, `opercmd` are ways to run other programs or
console commands, not sources of VSM data.  Nothing overlaps.

Rejected as sources, and why:

- Reading the CVT and its VSM control blocks directly.  It works (the PSA
  reads fine from a plain `-m32` program) but the offsets are an internal
  interface: they move between releases, and `CVTVSTGX` -- the structure
  that holds the PLPA/FLPA/MLPA extents -- is not reachable from a field
  the manual documents.
- SMF records (71, 78).  Writing them is a system decision, reading them is
  privileged, and the delay makes them useless for a live display.
- Console displays through `opercmd`.  Correct numbers, but it needs
  OPERCMDS authority and WLM, not a user program.

## Why assembler, and only there

`VSMLIST` is a macro: it builds a parameter list and issues an SVC, and
there is no C-visible entry point.  So `src/zvslist.s` is the whole of the
system interface, one page, and everything else -- parsing, formatting,
options -- is C23.  No control-block offsets are written down anywhere:
the macro brings its own.

The C side calls it with `#pragma linkage(zvslist, OS)`; `OS_UPSTACK`,
`__attribute__((OS))` and `OS31_NOSTACK` all abended at `-m32`, plain `OS`
does not.  HLASM upcases the entry name, so the C side binds `ZVSLIST` with
`__asm__("ZVSLIST")`.

Two things about the stub that are not obvious and cost a day each:

- In an XPLINK load module an assembler CSECT is read-only.  Reading a word
  out of it works; the first store takes a 0C4.  So the save area and every
  scratch word live in the caller's block (`struct zreq`), and the CSECT
  holds only code, the base register and the literal pool.
- `WKAREA=(4,5)` is read as an address constant, not a register: the macro
  then generates `LA 1,4` and stores its control block at address 12.  A
  register operand needs the doubled form, `WKAREA=((4),5)`.

## Language level

C23 for every project from here on: `-std=c23`, `constexpr`, `bool`,
`nullptr`, `__attribute__((aligned))` where it matters.  IBM Open XL 2.2
(clang 21) accepts `-std=c23`.

## Open

- The work-area layout.  The reference describes the descriptor stream in
  figures whose byte tables do not survive the documentation conversion, so
  `src/ztop.c` walks it with a plausibility check and `-x` dumps the raw
  bytes.  Next: confirm the framing against a dump on a live system, then
  replace the guess with the real rule.

  shortcut: the stream is scanned for a X'00' format byte, a sane length and
  a subpool id; a mis-parse shows up as an impossible subpool or a wild byte
  count, and `-x` is the way to see it.
- A refresh loop (`-d`, `-n`) and a `%`-of-area column need the area sizes,
  which VSMLIST does not return (it reports allocation, not the limit).
- `SP=PVT` output is grouped by owning TCB, which needs a different
  walk from the per-area one.
