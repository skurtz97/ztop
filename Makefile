# ztop -- storage areas on z/OS, at a glance.
#
# z/OS only.  This tree is kept on Linux as a backup copy, which is why
# `make` there stops with a word about it instead of failing obscurely.

CC      ?= ibm-clang
AS      ?= as
CFLAGS  ?= -std=c23 -O2 -m32 -Wall -Wextra -Wshadow -Wpedantic
LDFLAGS ?=

OBJ = src/zvslist.o

all: ztop

ztop: src/ztop.c $(OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ src/ztop.c $(OBJ)

src/zvslist.o: src/zvslist.s
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
