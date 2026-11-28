# libtui - a cell-buffer terminal UI with no ncurses.
#
# shellcheck disable=SC2283,SC2068
# ^ our linter reads this as shell: `NAME = value` and `$(CFLAGS)` are make.
#
#   make            build libtui.a
#   make demo       build the demo (a terminal is needed to run it)
#   make test       build and run the buffer tests
#   make clean
#
# On z/OS: make CC=ibm-clang.  The library itself is portable POSIX; the
# z/OS specifics (EBCDIC literals, the ESC byte) are all inside tui.c.
.POSIX:
.SUFFIXES:

CC     = cc
CFLAGS = -std=c23 -O2 -Wall -Wextra -Wshadow -Wpedantic \
         -D_XOPEN_SOURCE=500 -D_POSIX_C_SOURCE=200112L
AR     = ar

all: libtui.a

libtui.a: tui.o
	rm -f $@
	$(AR) rcs $@ tui.o

tui.o: tui.c tui.h
	$(CC) $(CFLAGS) -c -o $@ tui.c

demo: demo.c libtui.a
	$(CC) $(CFLAGS) -o $@ demo.c libtui.a

test: test_tui.c libtui.a
	$(CC) $(CFLAGS) -o test_tui test_tui.c libtui.a
	./test_tui

clean:
	rm -f tui.o libtui.a demo test_tui
