# ztop

What z/OS has where, from a z/OS UNIX shell over SSH: storage by subpool,
the system's identity, and one line per address space.

```text
area csa: used 71018616 free 526216 mapped 71544832 blocks 327 unallocated 194879488 total 266424320 use 27%
 sp key   blks       used       free     mapped    largest           address
227   0     29    1607848      59224    1667072     335872 00B53000-17CF9000
228   0     54    2149808      74320    2224128     823296 00B3F000-17CFE000
231   0     78    3358584      65672    3424256    1077248 00B54000-17CE8000
241   0    166   63902376     327000   64229376   26689536 00B38000-17D00000
unallocated 194879488 bytes in 5 blocks, largest 191475712 regions 00800000+0039C000, 08287000+0FA79000

system CBT  host CBT  os OS/390 29.00  machine 3932  address spaces 105/355

asid job          ascb
   1 *MASTER* 00FD7400
   2 PCAUTH   00FC7780
   3 RASP     00FC7600
```

Storage comes from `VSMLIST`, the interface z/OS offers for exactly this.
It is an assembler macro with no C equivalent, so `src/zvslist.S` issues it
and `src/vsm.c` parses the answer.  That needs no APF bit: the default
`LINKAGE=SYSTEM` path runs in problem state.

The system name and the address spaces come from the CVT and the ASVT,
read directly as documented programming interfaces (`src/sys.c`): no
service, no authorization.

`tui/` is the screen library: a cell buffer with a diffing flush, a
message loop shaped like Bubble Tea (model, update, view, ticks) and
widgets for a viewport, a table, tabs, a text input and a status line.
`src/ztop.c` is one user of it, not part of it.

## Build

z/OS for the real binary, with IBM Open XL in the path:

```sh
make tag        # once after git checkout: tag the ASCII sources
make            # ibm-clang -std=c23 -m32; the .S is assembled by HLASM
make check      # storage, system and jobs
make leakcheck  # every mode under the LE storage and heap checks
```

`make tag` matters because git leaves the files untagged, and the z/OS
tools then read ASCII as EBCDIC: make spins on the Makefile, `sh` rejects
the test script.

The parser, the library and `sys.c`'s structure have no z/OS dependency,
so on any host with a C compiler:

```sh
make test TESTCC=cc     # synthetic VSMLIST bytes and library buffers
make demo TESTCC=cc     # the library on its own
```

## Use

```text
ztop                    full-screen view when stdout is a terminal,
                        else one-shot CSA
ztop -a sqa             SQA, -a lsqa, -a pvt
ztop -a csa,pvt         several areas
ztop -s                 system name, host, release, address-space count
ztop -j                 one line per address space
ztop -x                 dump the raw VSMLIST work and data areas
ztop -d [seconds]       interactive, 2 s refresh (1..60)
```

With a terminal and no report option, `ztop` starts the full-screen view.
Anything that selects a report (`-a`, `-s`, `-j`, `-x`) or pipes the output
gets the one-shot text instead, suitable for `grep` and for a shell
prompt.  There are three views, selected with `TAB`:

| view | |
| --- | --- |
| STORAGE | CSA, SQA, LSQA and the private area, one table per area |
| SYSTEM | CVT system name, host, release, machine, address-space count |
| JOBS | ASID, job name and ASCB address for every address space |

| key | |
| --- | --- |
| `TAB`, `Shift-TAB`, click a tab | next / previous view |
| `1`..`4` | inside STORAGE: CSA, SQA, LSQA, private |
| `p` | pause/resume refresh |
| `r` | collect now |
| `+`/`-` | refresh 1..60 s |
| `j`/`k`, arrows, `PgUp`/`PgDn`, `g`/`G` | move and scroll the selected table |
| wheel, click a row | scroll, select a row |
| `s`, `S` | next sort column, reverse it (the header shows both) |
| `Enter` | every field of the selected subpool |
| `?` | help |
| `q` | quit |

Each table remembers its own selection and scroll position, so switching
views does not lose your place.  The status line shows the subpool or
address space under the cursor.  The design, including why the data
sources are what they are, is in [DESIGN.md](DESIGN.md).

## What it is not

A snapshot, not an accounting tool: no history, no per-job attribution, no
SMF.  CSA has a total because `VSMLIST` returns the unallocated holes as
well; SQA, LSQA and the private area do not, so those views show
used/free/mapped and no percentage.  The jobs view does not show CPU time:
`ASCBEJST` does not advance while a process burns CPU (measured), so a
column built on it would lie.
