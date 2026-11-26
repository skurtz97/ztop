# ztop

How much storage is in each area of a z/OS system -- CSA, SQA, LSQA, and
the private area -- from a z/OS UNIX shell over SSH.

```text
subpool  key  blocks        used        free      mapped     largest     address
227        8      12     1048576      196608     1245184     1048576  00A00000-00B30000
231        8      41    16777216     2097152    18874368     4194304  00C00000-02C00000
CSF total: 17825792 bytes, 17121280 mapped (96.2% of mapped+unallocated)
```

The numbers come from `VSMLIST`, the interface z/OS offers an authorized
program for exactly this.  It is an assembler macro with no C equivalent, so
`src/zvslist.s` issues it and `src/vsm.c` parses the answer; `src/tui.c` is
the terminal layer and `src/ztop.c` is the command.

## Build

z/OS only for the real binary, with IBM Open XL and HLASM in the path:

```sh
chtag -t -c ISO8859-1 Makefile   # after a checkout: make reads it as text
make          # ibm-clang -std=c23 -m32, plus `as` for src/zvslist.s
make apf      # then give the executable the APF bit
```

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
