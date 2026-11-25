# ztop

How much storage is in each area of a z/OS system, from a z/OS UNIX shell.

```
subpool      bytes   blocks
227        1048576       12
231       16777216       41
total     17825792
```

The numbers come from `VSMLIST`, the interface z/OS offers an ordinary
user task (problem state, key 8, `LINKAGE=SYSTEM`) for exactly this.  It
is an assembler macro with no C equivalent, so `src/zvslist.s` issues it
and `src/ztop.c` parses and formats the answer.

## Build

z/OS only, with IBM Open XL and HLASM in the path:

```sh
make          # ibm-clang -std=c23 -m32, plus `as` for src/zvslist.s
./ztop        # CSA
./ztop -a sqa # SQA, -a lsqa, -a pvt, -x dumps the raw work area
```

On Linux `make` stops with a message: this tree is kept there as a backup
copy, not to build.  Keep the two in step with `git` -- one repository,
cloned in `~/src/ztop` on both machines.

## What it is not yet

A snapshot, not a monitor: no refresh loop, no `%`-of-limit column, no
per-TCB private breakdown.  [DESIGN.md](DESIGN.md) says what comes next and
why the data source is what it is.
