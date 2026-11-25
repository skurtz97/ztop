# ztop design

Decisions, not a wish list.  Anything not yet measured says so.

## Where the numbers come from

`VSMLIST` (MVS Authorized Assembler Services Reference, chapter 30).  It is
the only documented interface that reports allocated storage by subpool for
CSA, SQA, LSQA and the private area, and its caller requirement is problem
state, PSW key 8-15, `LINKAGE=SYSTEM`, AMODE 24 or 31 -- an ordinary user
task.  It returns, per subpool, the 4K-multiple blocks that hold allocations
and the bytes allocated inside them.

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

The C side calls it with `#pragma linkage(zvslist, OS_UPSTACK)`: at `-m32`
plain `OS` means "no save area" and a traditional upward-stack service
abends on the first call (measured in zash, `src/sys_zos.c:585`).

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
