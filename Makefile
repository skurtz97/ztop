# shellcheck disable=SC2283,SC2046,SC2068,SC1089
# ^ our linter reads this as shell: `NAME = value` and `$(CFLAGS)` are make.
.POSIX:
.SUFFIXES:
.SUFFIXES: .c .s .o

# ztop - what VSM has allocated, by subpool.
#
#   make            build ./ztop
#   make apf        build it and give the executable the APF bit
#   make check      run it against each area
#   make clean      remove what the targets above write
#
# z/OS only: VSMLIST and the PSA do not exist elsewhere, so `make` on
# another host says so and stops.  This tree is kept on Linux as a backup.
#
# The build is POSIX make, the make z/OS ships: no :=, no ?=, no ifeq,
# no $(shell), no pattern rules.  gmake is not installed everywhere and
# /bin/make is not GNU make.

CC      = ibm-clang
AS      = as
CFLAGS  = -std=c23 -O2 -m32 -Wall -Wextra -Wshadow -Wpedantic
# AC(1) is half of what makes an executable APF-authorized, the other
# half being the APF bit on the file (make apf).  Quoted: the z/OS shell
# reads `(` in an unquoted word as a syntax error.
LDFLAGS = -Wl,'-bAC(1)'

OBJ = src/zvslist.o

all:
	@uname -s | grep OS/390 >/dev/null || \
	    { echo "ztop builds only on z/OS; this tree is a backup copy"; exit 1; }
	@exec $(MAKE) ztop

ztop: src/ztop.c $(OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ src/ztop.c $(OBJ)

# The assembler reads HFS source through the file's coded character set
# tag; a git checkout arrives tagged UTF-8 and HLASM then reads the file
# as if it were EBCDIC ("SYNAD ... WRONG LEN RECRD").  Tag it as ASCII,
# and keep every statement inside column 71: column 72 is the
# continuation mark, and a long comment turns the next line into one.
.s.o:
	chtag -t -c ISO8859-1 $<
	@awk 'length > 71 { print FILENAME ":" NR ": past column 71"; bad = 1 } \
	      END { exit bad }' $<
	$(AS) -o $@ $<

# A relink drops the APF bit, so run this after every build.
apf: ztop
	extattr +a ztop
	ls -E ztop

check: ztop
	./ztop
	./ztop -a sqa
	./ztop -a pvt

clean:
	rm -f ztop $(OBJ)
