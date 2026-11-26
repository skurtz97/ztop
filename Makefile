# shellcheck disable=SC2283,SC2046,SC2068,SC1089
# ^ our linter reads this as shell: `NAME = value` and `$(CFLAGS)` are make.
.POSIX:
.SUFFIXES:

# ztop - what VSM has allocated, by subpool.
#
#   make            build ./ztop
#   make check      run it against each area
#   make clean      remove what the targets above write
#
# z/OS only: VSMLIST and the PSA do not exist elsewhere, so `make` on
# another host says so and stops.  This tree is kept on Linux as a backup.
#
# The build is POSIX make, the make z/OS ships: no :=, no ?=, no ifeq,
# no $(shell), no pattern rules.  gmake is not installed everywhere and
# /bin/make is not GNU make.  One more z/OS make trait to know about: a
# recipe line holding a pipeline *and* a || spins it, so the pipeline is
# parenthesised (make/tests on the ztop machine depend on that).

CC      = ibm-clang
CFLAGS  = -std=c23 -O2 -m32 -Wall -Wextra -Wshadow -Wpedantic \
          -D_XOPEN_SOURCE=500 -D_POSIX_C_SOURCE=200112L
# The parser and the TUI framework have no z/OS dependency; the test and
# demo targets build them on the host so a Linux tree can check them.
TESTCC  = ibm-clang
TESTFLAGS = -std=c17 -g -O1 -Wall -Wextra -Wshadow -Wpedantic \
            -D_XOPEN_SOURCE=500 -D_POSIX_C_SOURCE=200112L
# AC(1) selects XPLINK.  Quoted: the z/OS shell reads `(` in an
# unquoted word as a syntax error.
LDFLAGS = -Wl,'-bAC(1)'

# The .S suffix sends the stub through clang's preprocessor; -P drops
# the line markers HLASM would read as statements, and -fno-integrated-as
# hands the result to HLASM.  That path writes its own tagged temporary,
# so the checked-out ASCII source needs no chtag first.
ASFLAGS = -fno-integrated-as -Xpreprocessor -P
SRC = src/ztop.c src/vsm.c src/tui.c
OBJ = src/zvslist.o
HDR = src/vsm.h src/tui.h

all:
	@(uname -s | grep OS/390 > /dev/null) || \
	    { echo "ztop builds only on z/OS; this tree is a backup copy"; exit 1; }
	$(MAKE) ztop

ztop: $(SRC) $(OBJ) $(HDR)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(SRC) $(OBJ)

src/zvslist.o: src/zvslist.S
	$(CC) $(CFLAGS) $(ASFLAGS) -c -o $@ src/zvslist.S

check: ztop
	./ztop
	./ztop -a sqa
	./ztop -a pvt

# Portable checks: no VSMLIST, no terminal.
test: tests/test_vsm.c src/vsm.c src/vsm.h
	$(TESTCC) $(TESTFLAGS) -o tests/test_vsm tests/test_vsm.c src/vsm.c
	./tests/test_vsm

demo: tests/demo_tui.c src/tui.c src/tui.h
	$(TESTCC) $(TESTFLAGS) -o tests/demo_tui tests/demo_tui.c src/tui.c

clean:
	rm -f ztop $(OBJ) tests/test_vsm tests/demo_tui
