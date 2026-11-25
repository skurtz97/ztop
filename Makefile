# ztop -- storage areas on z/OS, at a glance.
#
# z/OS only.  This tree is kept on Linux as a backup copy, which is why
# `make` there stops with a word about it instead of failing obscurely.
# Use gmake on z/OS: /bin/make is not GNU make and cannot read this file.

# CC is predefined by make, so ?= would keep /bin/cc.
CC      := ibm-clang
AS      ?= as
CFLAGS  ?= -std=c23 -O2 -m32 -Wall -Wextra -Wshadow -Wpedantic
LDFLAGS ?=

OBJ = src/zvslist.o

all: ztop

ztop: src/ztop.c $(OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ src/ztop.c $(OBJ)

# The assembler reads HFS source through the file's coded character set
# tag; a git checkout arrives tagged UTF-8 and HLASM then reads the file
# as if it were EBCDIC ("SYNAD ... WRONG LEN RECRD").  Tag it as ASCII.
src/zvslist.o: src/zvslist.s
	chtag -t -c ISO8859-1 src/zvslist.s
	$(AS) -o $@ src/zvslist.s

check: ztop
	./ztop
	./ztop -a sqa
	./ztop -a pvt

clean:
	rm -f ztop $(OBJ)

# Linux is a backup, not a build: VSMLIST and the PSA do not exist there.
ifeq ($(shell uname -s),Linux)
$(error ztop builds only on z/OS; on Linux this tree is a backup copy)
endif

.PHONY: all check clean
