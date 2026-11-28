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

The IPL data comes from the SMCA (`CVTSMCA`, CVT+0xC4): `SMCAIDTE` at +340
is packed decimal `0CYYDDDF` and `SMCAITME` at +336 is binary hundredths of
a second.  Both are decoded by `zipl_decode` in `src/sys.c` and checked
against `D IPLINFO` (2026-09-21 17:02:47).  `CVTRLSTG` (CVT+0x358) is real
storage in K at IPL; it is accurate below 4 TB and is a sentinel above.

The private area gets the same unallocated pass as CSA, so its total and
percentage are shown; SQA and LSQA are not asked for unallocated blocks,
and their totals are not knowable this way.  The paging view reads ASMVT
(`CVTASMVT`, CVT+0x2C0) for the common and PLPA page data sets' flags and
the VIO journal count; it does not read the data-set names yet.

## The UNIX views

Two views read z/OS UNIX, both in the problem state, both without
allocation.  `src/uss.c` holds the two readers; `src/uss.h` the two fixed
tables.

PS walks `w_getpsent` (BPX1GPS) from process token 0, which the callable
services manual defines as "the first process in the system", and calls
it again with each returned token until it answers 0 (no more processes
this caller may see) or -1 (error, errno in the return code).  The
columns are documented members of `W_PSPROC` in `<sys/ps.h>`: `ps_pid`,
`ps_ppid`, `ps_euid`, `ps_state` (letter in `PS_STATECHAR`, an `S`
suffix for `PS_SWAP`), and `ps_usertime` + `ps_systime` in
`CLOCKS_PER_SEC` units.  The service has no job name, no ASID and no
thread count, so PS and JOBS are not cross-linked and no column pretends
otherwise.

FS calls `w_getmntent` once with the mnt3 mapping -- `MNTE3H_ID` in
`mnt3H_cbid`, `sizeof(MNTE3)` in `mnt3H_cblen`, `sizeof(W_MNTENT3)` in
`mnt3H_bodylen` -- and takes the mount point, the FILESYS type name and
the aggregate data set name from the entries.  Size, free space and the
read-only flag then come from `statvfs()` per mount, not from
`mnt3_mode`: `ST_RDONLY` is the documented POSIX bit, while the mount
mode word would have to be decoded into meaning the C header does not
name.

The caps are `USS_MAX_FS 64` and `USS_MAX_PS 128`; on CBT the counts are
31 file systems and 60 processes.  The `w_getmntent` buffer is one
file-scope static of 64 `W_MNTENT3` and nothing at all is allocated; a
count past a cap sets `truncated`, which the footer prints instead of
hiding.  Each reader runs only when its view is on screen, so the tick
that refreshes STORAGE does not pay for them.

Unverified, and said so on purpose: the PID numbers are the raw `ps_pid`
values and were not cross-checked against `ps -ef`; the mount status byte
and the quiesce owner are not shown; there is no CPU percentage (only
total CPU time) and no per-process memory.

## The console

`MCSOPER` and `MCSOPMSG` are the only services ztop needs that no C
program can call: SA22-7614 puts both at "supervisor state and any PSW
key".  They live in `src/zmcs.s`, which mirrors `src/zvslist.S`: it
saves the caller's registers, points R2 at a request block, switches to
supervisor state for the call, switches back, and leaves the service
return code in the block.  The block is the C struct in `src/zmcs.h`,
and the offsets in the stub are its -m32 offsets (40 for the save area,
184 for the console status area); `src/ztop.c` asserts them under
`__MVS__`, because the same struct on a 64-bit host has a different
shape and the check would only be noise there.

The stub is plain assembler, not `.S`, and that is deliberate.  The `.S`
path sends the source through clang's preprocessor, and that stage
squeezes the runs of blanks which hold a continuation character in
column 72: a `MCSOPER` call with ten operands cannot survive it.  Two
minimal files settled it -- identical bytes, `.s` assembled and `.S`
failed with `ASMA141E Bad character in operation code` -- so the rule in
the Makefile is a second rule for this one file, with no preprocessor.

Opening the console is opt-in (`ztop -c`), because the failure mode is
not a return code.  On CBT, without APF, the first `MODESET MODE=SUP`
ended the task: `CEE3250C The system or user abend S047 R=NULL was
issued. From entry point log_start`.  A program that cannot enter
supervisor state does not get told so politely, so ztop does not try
unless asked; without `-c` the LOG view says `console off: ztop -c after
make auth`.  `make auth` runs `extattr +ap ./ztop`, which marks the
file; the library it sits in still has to be in the APF list, which is
an operator or system programmer action, and the user needs READ on
`MVS.MCSOPER.ZTOP`.

The console is a log, not a command line: no `OPERPARM` area is passed,
so it takes the session defaults (authority INFO, message level ALL,
scope *ALL), which is what reading wants.  Issuing commands is `MGCRE`,
which also needs OPERCMDS -- a different feature, not a switch.

A message lives in the console's data space, so the stub copies it out
while still in supervisor state: the copy length is the MDB's own
`MDBLEN` clipped to the caller's buffer (4096 bytes in the model), and
the C side then walks the object chain with every length checked against
the copy -- the bytes come from another address space.  The text object
(type 4) carries the line at +9, the general object (type 1) the job
name at +48 (IEAVM105).

Nothing allocates.  The ring is `LOG_LINES 256` lines of `LOG_COLS 200`
inside the model, so 51 KB of a static struct; a full ring overwrites
its oldest line and the footer says `oldest dropped`.  `MCSOPMSG` is
called at most 200 times per tick, which bounds a burst, and in the
steady state once -- the single call that returns RC 8 ("nothing
queued").  The console stays open when another view is on screen, so the
queue does not build up behind it, and it is closed on quit and after
`tui_run` returns, so a signal that ends the loop still deactivates it.

Unverified, and the reason the tab says so little: with APF in place and
MVS.MCSOPER.ZTOP granted, not one message has been read yet.  The
activate and get-msg paths have assembled, linked and aborted at
MODESET; nothing beyond that has run.

## The terminal

`tui/` is a small library, not a curses replacement: a cell buffer with a
diffing flush, a message loop and widgets.  The loop is the Elm shape
Bubble Tea made common -- the application owns a model, receives a
message (key, resize, tick) and redraws from the model -- because it keeps
the terminal code out of the application and gives every view the same
keys.  The widgets are a viewport with its own selection and scroll
offset, a table built from columns and a cell callback, tabs, a single
line input, a progress bar, a sparkline, a scrollbar and a status
line; an application that wants no
tabs simply never draws them.  The mouse is part of the message loop: SGR
mouse reporting (1006) turns a click into a `TUI_MSG_MOUSE` with 0-based
cells, and `tui_tabs_hit` answers which tab a column belongs to, so the
application decides what a click means.  Commands and subscriptions are not modelled:
a tick message and a return value saying "redraw" are enough here, and a
`Cmd` type in C would be a function pointer by another name.

`make leakcheck` runs every report mode under the Language Environment
storage report and compares the heaps against each other rather than
against their own free counts: LE's own storage is never freed, so the
sensitive comparison is that mode A ends with the same heap as mode B.
All modes currently end at 12 gets, 0 frees and 17776 bytes in use.

The buffer keeps one screen of cells and one of what is already on the
glass, and writes only the runs that differ, with one style change per
run.  The library is compiled into the program rather than linked from an
archive, but `tui/Makefile` can build `libtui.a` for reuse.  Glyphs are
7-bit: the frame and the meters use the DEC Special Graphics set (`l q k
x m j`, shade `a`) designated into G0 with `ESC ( 0` and released with
`ESC ( B`.  That is an ASCII escape sequence, which the EBCDIC-to-ASCII
output path does carry; a UTF-8 box character would not survive it, since
the path is a byte conversion and not a charset translation.  A terminal
whose `TERM` is not one we recognise gets `+ - | #` instead, because
`lqqqk` where a corner belongs is worse than a plain frame.  Selection is
a background, not reverse video: reverse video is what makes a table read
as curses.

A view is one titled panel across the body -- `+-- STORAGE ---...---+`
in the source, drawn as a box on a terminal that has DEC graphics -- with
the tab row above it, the machine's identity on the top bar and the keys
on the status line, so that every view has the same frame and the screen
reads as one program.  Numbers that carry a view (the area's
used/free/mapped/total and its share in use, the address-space count) sit
either on the panel's first row or in its bottom border, next to the
table; whatever does not fit at the current width is dropped rather than
truncated into the field beside it.

Four z/OS details are load-bearing:

- The escape character in the source has to be EBCDIC X'27'.  The tty
  converts EBCDIC to ASCII on output, so a C `"\x1b"` arrives as X'8F';
  X'27' arrives as the ESC the terminal expects.  `tui/tui.c` picks the
  byte with `#ifdef __MVS__`; input is not converted, so keys arrive as
  EBCDIC and are mapped there too.  The same conversion passes the DEC
  `ESC ( 0` designation through unchanged, which is why the glyph choice
  above costs nothing on z/OS.
- `TIOCGWINSZ` succeeds with zero columns and rows.  The size comes from a
  cursor-position report (`ESC[6n`, answer `ESC[row;colR`) with a timeout,
  then `COLUMNS`/`LINES`, then 80x24.  `SIGWINCH` is handled where the
  system sends it; every handler is installed with `sigaction`, not
  `signal`, because the z/OS runtime prefers it.
- `-D_XOPEN_SOURCE=500 -D_POSIX_C_SOURCE=200112L` is what exposes termios,
  `select` and `clock_gettime` in the z/OS headers; `nanosleep` and `usleep`
  are not declared, so the refresh wait is `select`.
- `.gitattributes` tags the whole working tree ISO8859-1 with one rule, the
  way the zopen projects do it (their `meta` project tags everything UTF-8,
  their git port tags everything ISO8859-1 through the portable
  `working-tree-encoding` attribute): no per-file exceptions, because a
  rule with exceptions is a rule someone will get wrong.  Sources read
  through the EBCDIC converter need that ASCII tag, and holding the tree to
  7-bit is also why the frame is ASCII in the source and only the terminal
  draws the box.  `make tag` retags a tree that predates the file.

## One table for the keys

The keys are a table of bindings (`tui_binding`: key, label, group, the
tabs it applies to, whether it fits in the footer).  The footer line and
the `?` overlay are both generated from that table, so a key cannot be
documented in one place and forgotten in the other, and the tab mask keeps
`/`, `j`/`k` and `s` out of views where they would do nothing.  The
application's key handling is one `switch`-free chain of comparisons in
`update()`; the table is a description of it, not a second implementation.

## Colour and layout

A cell carries a foreground and a background from the xterm-256 palette
(`TUI_COLOR_DEFAULT` is 255 and is emitted as `;39`/`;49` so a terminal
keeps its own default background).  The palette is chosen for a dark
terminal over SSH and is not configurable: no dependency, no theme file.
Geometry is computed once per frame from `tui_rect` primitives
(`tui_split_v`, `tui_split_h`, `tui_inset`, `tui_rect_hit`), and the same
rectangles are handed to the mouse hit-test, so a click cannot land on a
row the table did not draw.

## Storage, the 16M line and folding

`vsm_info` and `vsm_subpool` carry `below` and `above` on top of `used`: a
block is counted on the side of the 16M line its first byte is on, which
is why the two do not always add up to `used` for a block that straddles
it.  The private-area table can be folded per owning TCB: the folded rows
are rebuilt from `vsm_info.sub[]` after every collect (sort by TCB, one
linear pass to group, no allocation), carry `SUBS`, `USED`, `<16M`, `>16M`
and `LINE`, and `Enter` opens the subpools that make the row up.  The
filter narrows the same order arrays the table, the detail line, the popup
and the mouse read, so there is one place a row index is translated.

## C23 and C++ callers
Every build is `-std=c23`: `nullptr`, `constexpr`, `static_assert`, `bool`
and integer/enum-typed limits (`enum { TUI_MAX_W = 500 };`).  The library
types are `typedef struct name { ... } name;`, so code says `tui *t`, not
`struct tui *t`.  `tui/tui.h` wraps its declarations in `extern "C"`, so a
C++ program can include the library header; the portable tree is built
with the same flags the z/OS build uses.

One `constexpr` does not survive the target compiler: IBM Open XL 2.2
rejects `static constexpr char ZTOP_VERSION[] = "0.3";` with "constexpr
initializer evaluates to -16 which is not exactly representable in type
'const char'", where clang 23 on Linux accepts it.  That constant stays a
plain `static const char[]`; the rest of the tree's `constexpr` use is
scalar and builds on both.

## Tidy findings that stand

`make tidy` is the standard, and the tree still has four groups of findings
that predate it.  They are recorded here so that a change touching these
files can say why they remain rather than being silently carried:

- `bugprone-signed-bitwise` on `attr & TUI_BOLD` and the `flags & 0x0F`
  tests: the operand is `unsigned char` and integer promotion makes the
  operator signed.  The values are small, the intent is a bit test, and the
  flag mask is masked again before use; an explicit `unsigned` cast at
  every site would add noise without changing a result.
- `bugprone-unchecked-string-to-number-conversion` on `atoi(COLUMNS)` and
  `atoi(LINES)` in the terminal size fallback.  A terminal size is
  advisory: a malformed value falls back to 80x24, which is the behaviour
  wanted, and `strtol` with an error path would not be better.
- `clang-analyzer-security.ArrayBound` at the `region[]` guard in
  `src/vsm.c:259`: one analyzer false positive, the bound is checked
  immediately before the access.  (The second one this note used to
  mention, at the one-cell `tui_put` call, went away when `tui_sparkline`
  started passing a one-character string.)
- `cert-int09-c` for the key enum before this change: now every enumerator
  has its value written out, which is also what makes the order irrelevant.

## Open

- Live confirmation was the first acceptance test and passed: `ztop -a
  csa`, `-a sqa`, `-a lsqa` and `-a pvt` returned data on CBT with the
  parser reporting no warning.  The 4K-block, free-descriptor and
  unallocated streams match the layout above.
- Area sizes for SQA, LSQA and the private area.  `VSMLIST` reports
  allocation and free space, not the limit; a percentage there needs a
  different source, and none is planned.
- `SP=PVT` folding by owning TCB (`t`), the inspector pane (`i`), the
  filter (`/`) and the command line (`:`) are exercised on a Linux pty with
  synthetic data only; the folded table has not been seen against a real
  `VSMLIST` PVT answer yet.
- The full-screen view has been exercised on a Linux pty and a z/OS pty:
  view switching, area switching, scrolling, help, quit and the cursor
  restore.  What has not been seen is a real xterm over SSH answering the
  size query; a terminal that ignores `ESC[6n` falls back to
  `COLUMNS`/`LINES` or 80x24.
- The jobs view lists every address space and its ASID and ASCB; job name
  lookup uses the documented `ASCBJBNS` pointer.  Sorting, filtering and
  per-job storage are not implemented.
- CPU-measurement counters.  The machine has the facility: a probe read
  `STFLE` bit 67 (counter facility), 68 (sampling) and 142 (store CPU
  counter multiple) on CBT on 2026-11-26 and found all three set.  A
  problem-state program still may not read a counter: the same probe took
  the privileged-operation program check at `ECCTR` (`S0C2`, reason 2),
  which is what SA23-2260-08 p.2-27 specifies while bit 15 of control
  register 0 is zero - its initialised value, p.2-1, and a bit only the
  installation sets.  A counters view would therefore need an authorised
  caller or a change to the system, not code in `ztop`.  Counter meaning
  for this z16, if it ever becomes readable, is SA23-2261-09, numbers 128
  to 287.
