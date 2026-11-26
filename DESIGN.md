# ztop design

Decisions, not a wish list.  Anything not yet measured says so.

## Where the numbers come from

`VSMLIST` (MVS Authorized Assembler Services Reference, chapter 30).  It is
the only documented interface that reports allocated storage by subpool for
CSA, SQA, LSQA and the private area.  Per subpool it returns the 4K-multiple
blocks that hold allocations and the bytes allocated inside them.

## Authorization

Not needed, as it turned out.  `VSMLIST` was called with the default
`LINKAGE=SYSTEM` (PC-number path, problem state, PSW key 8) from an
ordinary HFS executable with no APF bit, and returned data.  `D PROG,APF`
on CBT lists datasets only, so no HFS path could have been authorized
anyway.

The earlier "SC78 R=90 on an unauthorized program" reading was wrong: the
abend came from a malformed work area.  `WKAREA=(4,5)` is an address
constant, not a register pair, so the macro stored the literal 4 as the
subpool and the literal 5 as the work-area length, then wrote its control
block over the request.  With `WKAREA=((4),(5))` the service is called
correctly and does not look at the caller's authority.

What still matters is that the program only reads VSM data and writes to
standard output; it must not grow a feature that takes a path, a command
or a name from an untrusted caller.

## What ZOAU already provides

Checked so it is not duplicated: ZOAU 1.4 ships no storage or subpool
facility.  `zinfo` reports IPL, CPU, system and IODF information only;
`apfadm`, `mvscmd`, `mvscmdauth`, `opercmd` are ways to run other programs or
console commands, not sources of VSM data.  Nothing overlaps.

Rejected as sources, and why:

- Reading the CVT and its VSM control blocks directly, for storage.  It
  works (the PSA reads fine from a plain `-m32` program) but those offsets
  are an internal interface, and `CVTVSTGX` -- the structure that holds the
  PLPA/FLPA/MLPA extents -- is not reachable from a field the manual
  documents.  `VSMLIST` names what it returns; the VSM control blocks do
  not.  The CVT and ASVT are read for the jobs view, where the fields used
  are listed as programming interfaces and no callable service exists.
- SMF records (71, 78).  Writing them is a system decision, reading them is
  privileged, and the delay makes them useless for a live display.
- Console displays through `opercmd`.  Correct numbers, but it needs
  OPERCMDS authority and WLM, not a user program.

## Why assembler, and only there

`VSMLIST` is a macro: it builds a parameter list and calls a PC service,
and there is no C-visible entry point.  So `src/zvslist.S` is the whole of
the system interface, one page, and everything else -- parsing, formatting,
options -- is C23.  No control-block offsets are written down anywhere:
the macro brings its own.  The `.S` suffix makes ibm-clang assemble the
file in-process; a separate `as` wants the source tagged and converted
to EBCDIC first.

The C side calls it with `#pragma linkage(zvslist, OS)`; `OS_UPSTACK`,
`__attribute__((OS))` and `OS31_NOSTACK` all abended at `-m32`, plain `OS`
does not.  HLASM upcases the entry name, so the C side binds `ZVSLIST` with
`__asm__("ZVSLIST")`.

Three things about the stub that are not obvious and cost a day each:

- In an XPLINK load module an assembler CSECT is read-only.  Reading a word
out of it works; the first store takes a 0C4.  So the save area and every
scratch word live in the caller's block (`struct zreq`); the CSECT holds
only code and the literal pool.
- In an XPLINK load module the binder turns `=A(zvslist)` into a function
descriptor, and the loader fills that with an address that is not the
CSECT.  Loading it into the base register made every literal and branch
go elsewhere (0C1, 0C4 or 0C7 depending on what happened to be there).
`BALR 12,0` plus the assembler's own `USING` is the whole of the base
setup; no address constant is needed.
- `WKAREA=(4,5)` is read as an address constant, not a register: the macro
then generates `LA 15,5` and stores the literal 5 as the length.  A
register operand needs the doubled form, `WKAREA=((4),(5))`.

`SPACE` is not a runtime operand of the macro: FREE and UNALLOC are two
different expansions, which is why the stub branches to one or the other
before issuing it.

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

## System and jobs

The CVT gives the system name (`CVTSNAME`, CVT+0x154) and the ASVT
(`CVTASVT`, CVT+0x22C).  The ASVT holds `ASVTMAXU` at +0x204 and the
entries at +0x210; bit 0 of an entry means the ASID is free, otherwise it
is the ASCB address.  From the ASCB the view takes `ASCBASID` (halfword at
+0x24) and `ASCBSRBT`'s pointer `ASCBJBNS` (fullword at +0xB0) for the
job name.  All of these are in the data-areas manual as programming
interfaces, and none needs authorization.

The fields are read as bytes with `memcpy`, never by casting an integer to
a pointer, so a misaligned read cannot fault.  The strings stay EBCDIC:
the program is compiled in EBCDIC mode and the terminal converts on
output, so converting them first would double-convert.  This was measured
the other way round first -- `__e2a_l` in the middle produced mojibake on
the terminal and is not used.

CPU time is deliberately absent: `ASCBEJST` did not advance while a test
process burned 0.88 s of CPU (measured on CBT), so it is not a live field.

## The terminal

`tui/` is a small library, not a curses replacement: a cell buffer with a
diffing flush, a message loop and widgets.  The loop is the Elm shape
Bubble Tea made common -- the application owns a model, receives a
message (key, resize, tick) and redraws from the model -- because it keeps
the terminal code out of the application and gives every view the same
keys.  The widgets are a viewport with its own selection and scroll
offset, a table built from columns and a cell callback, tabs, a single
line input, a progress bar and a status line; an application that wants no
tabs simply never draws them.  The mouse is part of the message loop: SGR
mouse reporting (1006) turns a click into a `TUI_MSG_MOUSE` with 0-based
cells, and `tui_tabs_hit` answers which tab a column belongs to, so the
application decides what a click means.  Commands and subscriptions are not modelled:
a tick message and a return value saying "redraw" are enough here, and a
`Cmd` type in C would be a function pointer by another name.

The buffer keeps one screen of cells and one of what is already on the
glass, and writes only the runs that differ, with one style change per
run.  The library is compiled into the program rather than linked from an
archive, but `tui/Makefile` can build `libtui.a` for reuse.  No Unicode:
the box and the bar are `+`, `-`, `|`, `#`, because the terminal path is
an EBCDIC-to-ASCII conversion that is not a charset translation.

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

- Live confirmation was the first acceptance test and passed: `ztop -a
  csa`, `-a sqa`, `-a lsqa` and `-a pvt` returned data on CBT with the
  parser reporting no warning.  The 4K-block, free-descriptor and
  unallocated streams match the layout above.
- Area sizes for SQA, LSQA and the private area.  `VSMLIST` reports
  allocation and free space, not the limit; a percentage there needs a
  different source, and none is planned.
- `SP=PVT` output is already grouped by owning TCB, but the view still
  prints one list; per-TCB folding is a display change, not a data change.
- The full-screen view has been exercised on a Linux pty and a z/OS pty:
  view switching, area switching, scrolling, help, quit and the cursor
  restore.  What has not been seen is a real xterm over SSH answering the
  size query; a terminal that ignores `ESC[6n` falls back to
  `COLUMNS`/`LINES` or 80x24.
- The jobs view lists every address space and its ASID and ASCB; job name
  lookup uses the documented `ASCBJBNS` pointer.  Sorting, filtering and
  per-job storage are not implemented.
