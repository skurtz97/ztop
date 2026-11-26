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

all:
	@(uname -s | grep OS/390 > /dev/null) || \
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

test: tests/test_vsm.c src/vsm.c tui/test_tui.c tui/tui.c
	$(TESTCC) $(TESTFLAGS) -o tests/test_vsm tests/test_vsm.c src/vsm.c
	./tests/test_vsm
	$(TESTCC) $(TESTFLAGS) -Itui -o tui/test_tui tui/test_tui.c tui/tui.c
	./tui/test_tui

demo: tui/demo.c tui/tui.c
	$(TESTCC) $(TESTFLAGS) -Itui -o tui/demo tui/demo.c tui/tui.c

clean:
	rm -f ztop $(OBJ) tests/test_vsm tui/test_tui tui/demo
