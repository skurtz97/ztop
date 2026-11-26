# shellcheck disable=SC2283,SC2046,SC2068,SC1089
# ^ our linter reads this as shell: `NAME = value` and `$(CFLAGS)` are make.
.POSIX:
.SUFFIXES:
.SUFFIXES: .c .o

# ztop - what z/OS has where, for a systems programmer over ssh.
#
#   make            build ./ztop (z/OS)
#   make check      run it against storage, system and jobs
#   make test       parser, library and system helpers, on any host
#   make demo       tui/demo, the library on its own
#   make clean      remove what the targets above write
#
# z/OS only for ./ztop: VSMLIST, the PSA and the ASVT do not exist
# elsewhere, so `make` on another host says so and stops.  This tree is
# kept on Linux as a backup and for the portable tests.
#
# The build is POSIX make, the make z/OS ships: no :=, no ?=, no ifeq,
# no $(shell), no pattern rules.  One more z/OS make trait: a recipe line
# holding a pipeline *and* a || spins it, so the pipeline is
# parenthesised.

CC       = ibm-clang
CSTD     = -std=c23
WARN     = -Wall -Wextra -Wshadow -Wpedantic
CFLAGS   = $(CSTD) $(WARN) -O2 -m32 -D_XOPEN_SOURCE=500 \
           -D_POSIX_C_SOURCE=200112L
CPPFLAGS = -Itui
# AC(1) selects XPLINK.  Quoted: the z/OS shell reads `(` in an
# unquoted word as a syntax error.
LDFLAGS  = -Wl,'-bAC(1)'

# The .S suffix sends the stub through clang's preprocessor; -P drops
# the line markers HLASM would read as statements, and -fno-integrated-as
# hands the result to HLASM.  That path writes its own tagged temporary,
# so the checked-out ASCII source needs no chtag first.
ASFLAGS  = -fno-integrated-as -Xpreprocessor -P

# The library is compiled into the program rather than linked as an
# archive: one binary, and the same sources build on Linux for the tests.
SRC = src/ztop.c src/vsm.c src/sys.c tui/tui.c
OBJ = src/zvslist.o
HDR = src/vsm.h src/sys.h tui/tui.h

# The portable half: no z/OS services, no terminal.
TESTCC    = cc
TESTFLAGS = -std=c17 -g -O1 -Wall -Wextra -Wshadow -Wpedantic \
            -D_XOPEN_SOURCE=500 -D_POSIX_C_SOURCE=200112L

# Language Environment sees every heap request the program makes.  Its
# storage report is therefore the leak test a z/OS user can run: if the
# gets and the frees differ by the end of the enclave, something was
# allocated and never freed.  HEAPCHK(ON) additionally walks the heap at
# every call, so a double free or an overlay abends instead of hiding.

# git checkout leaves every source untagged, and the z/OS tools then read
# ASCII through the EBCDIC converter: make spins on the Makefile, sh
# rejects the script, awk reads garbage.  Run this once after a checkout.
tag:
	@-chtag -t -c ISO8859-1 Makefile README.md DESIGN.md tests/*.sh tests/*.py
	@-chtag -t -c ISO8859-1 src/*.c src/*.h src/*.S tui/*.c tui/*.h
	@-chtag -t -c ISO8859-1 tui/Makefile tui/test_tui.c tui/demo.c

all:
	@uname -s | grep OS/390 > /dev/null || \
	    { echo "ztop builds only on z/OS; this tree is a backup copy"; exit 1; }
	$(MAKE) ztop

ztop: $(SRC) $(OBJ) $(HDR)
	$(CC) $(CFLAGS) $(CPPFLAGS) $(LDFLAGS) -o $@ $(SRC) $(OBJ)

src/zvslist.o: src/zvslist.S
	$(CC) $(CFLAGS) $(ASFLAGS) -c -o $@ src/zvslist.S

check: ztop
	./ztop
	./ztop -a sqa
	./ztop -a pvt
	./ztop -j

test: tests/test_vsm.c tests/test_sys.c src/vsm.c src/sys.c tui/test_tui.c tui/tui.c
	$(TESTCC) $(TESTFLAGS) -o tests/test_vsm tests/test_vsm.c src/vsm.c
	./tests/test_vsm
	$(TESTCC) $(TESTFLAGS) -Isrc -o tests/test_sys tests/test_sys.c src/sys.c
	./tests/test_sys
	$(TESTCC) $(TESTFLAGS) -Itui -o tui/test_tui tui/test_tui.c tui/tui.c
	./tui/test_tui
	@sh tests/leakcheck.sh --selftest

demo: tui/demo.c tui/tui.c
	$(TESTCC) $(TESTFLAGS) -Itui -o tui/demo tui/demo.c tui/tui.c

leakcheck: ztop
	@sh tests/leakcheck.sh ./ztop

clean:
	rm -f ztop $(OBJ) tests/test_vsm tests/test_sys tui/test_tui tui/demo
