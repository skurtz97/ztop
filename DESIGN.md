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

## The shape of the answer

`VSMLIST` fills a caller-supplied work area.  The first 32 bytes are a
control block: return code at 0, then the address (4) and length (8) of a
data area placed after it.  The data area is in MVS byte order, so
`src/vsm.c` reads it big-endian and never casts.

The stream is a count followed by fixed 8-byte address/length pairs, at
every level: number of subpools, then per subpool an 8-byte descriptor
(format, descriptor length, subpool id, flags, owning TCB in the private
area), then a block count and the blocks.  With `SPACE=FREE` each allocated
block is followed by a free-descriptor count and the free extents inside
it; allocated bytes are the block length minus those free extents.  With
`SPACE=UNALLOC` the stream is regions, and inside each region the holes.

The call is a continuation: return code 4 means "did not fit, issue the
same call with the same work area again", and each call reports only its
own piece.  The return code at work+0 must be zero before the first call
and left untouched between the continuation calls; clearing it each time
restarts the request and never finishes.  `src/vsm.c` therefore merges the
pieces until the service reports zero, and `ztop -x` dumps the header and
the first bytes of each piece.

What `VSMLIST` does not return is the area size.  CSA can still be shown as
a percentage because the unallocated request returns the holes, so
footprint plus holes is the mapped extent; SQA, LSQA and the private area
have no such request and show used, free and mapped without a total.

## The terminal

A cell buffer and a diff flush, not curses.  ncurses brings its own
terminfo database, which is another thing to install and keep in step on
every z/OS system, and its output is a stream of absolute cursor motion
anyway.  `src/tui.c` keeps one screen of cells and one of what is already
on the glass, and writes only the runs that differ, with one style change
per run.  No Unicode: the box and the bar are `+`, `-`, `|`, `#`, because
the terminal path is an EBCDIC-to-ASCII conversion that is not a charset
translation.

Three z/OS details are load-bearing:

- The escape character in the source has to be EBCDIC X'27'.  The tty
  converts EBCDIC to ASCII on output, so a C `"\x1b"` arrives as X'8F';
  X'27' arrives as the ESC the terminal expects.  `src/tui.c` picks the
  byte with `#ifdef __MVS__`; input is not converted, so keys arrive as
  EBCDIC and are mapped there too.
- `TIOCGWINSZ` succeeds with zero columns and rows.  The size comes from a
  cursor-position report (`ESC[6n`, answer `ESC[row;colR`) with a timeout,
  then `COLUMNS`/`LINES`, then 80x24.  `SIGWINCH` is handled where the
  system sends it.
- `-D_XOPEN_SOURCE=500 -D_POSIX_C_SOURCE=200112L` is what exposes termios,
  `select` and `clock_gettime` in the z/OS headers; `nanosleep` and `usleep`
  are not declared, so the refresh wait is `select`.

## Open

- Live confirmation.  The parser follows the documented layout and is
  covered by synthetic data areas in `tests/test_vsm.c`, but no real
  `VSMLIST` answer has been parsed yet: the executable is APF-marked and
  still abends SC78 R=90 because no HFS directory is APF-listed.  The
  first run on an authorized system is the acceptance test for the framing.
- Area sizes for SQA, LSQA and the private area.  `VSMLIST` reports
  allocation and free space, not the limit; a percentage there needs a
  different source, and none is planned.
- `SP=PVT` output is already grouped by owning TCB, but the view still
  prints one list; per-TCB folding is a display change, not a data change.
