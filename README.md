# ztop

How much storage is in each area of a z/OS system -- CSA, SQA, LSQA, and
the private area -- from a z/OS UNIX shell over SSH.

```text
area csa: used 71014984 free 525752 mapped 71540736 blocks 326 unallocated 194883584 total 266424320 use 27%
 sp key   blks       used       free     mapped    largest           address
227   0     29    1607848      59224    1667072     335872 00B53000-17CF9000
228   0     54    2149808      74320    2224128     823296 00B3F000-17CFE000
231   0     77    3354952      65208    3420160    1077248 00B54000-17CE8000
241   0    166   63902376     327000   64229376   26689536 00B38000-17D00000
unallocated 194883584 bytes in 5 blocks, largest 191475712 regions 00800000+0039C000, 08287000+0FA79000
```

The numbers come from `VSMLIST`, the interface z/OS offers for exactly
this.  It is an assembler macro with no C equivalent, so
`src/zvslist.S` issues it and `src/vsm.c` parses the answer; `src/tui.c` is
the terminal layer and `src/ztop.c` is the command.

## Build

z/OS only for the real binary, with IBM Open XL in the path:

```sh
make          # ibm-clang -std=c23 -m32; the .S is assembled in-process
make check    # run it against each area
```

No APF bit and no special directory: `VSMLIST` was called successfully
from an ordinary HFS executable without `extattr`.  (An earlier build did
abend SC78 R=90, but that was a malformed work area: the macro stores the
length itself unless the register is doubly parenthesised.)

The parser and the TUI framework have no z/OS dependency, so on any host
with a C compiler and a terminal:

```sh
make test TESTCC=cc     # synthetic VSMLIST data areas, no z/OS needed
make demo TESTCC=cc     # a small exercise of the TUI framework
```

## Use

```text
ztop                    one-shot CSA
ztop -a sqa             SQA, -a lsqa, -a pvt
ztop -a csa,pvt         several areas
ztop -x                 dump the raw work area of the first call
ztop -d [seconds]       interactive, 2 s refresh (1..60)
```

Without `-d` the exact bytes of the answer go to standard output, suitable
for `grep` and for a shell prompt.  With `-d` there is a full-screen view:

| key | |
| --- | --- |
| `1`..`4` | CSA, SQA, LSQA, private |
| `a` | all four areas |
| `p` | pause/resume refresh |
| `r` | collect now |
| `+`/`-` | refresh 1..60 s |
| `j`/`k`, `PgUp`/`PgDn`, `g`/`G` | scroll a subpool list |
| `?` | help |
| `q` | quit |

The design, including why the data source is what it is and what is not
verified on z/OS, is in [DESIGN.md](DESIGN.md).

## What it is not

A snapshot, not an accounting tool: no history, no per-job attribution, no
SMF.  CSA has a total because `VSMLIST` returns the unallocated holes as
well; SQA, LSQA and the private area do not, so those views show
used/free/mapped and no percentage.
