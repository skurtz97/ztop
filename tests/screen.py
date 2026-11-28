#!/usr/bin/env python3
"""Paint ztop into a grid and print it, to check the layout without an eye.

    python3 tests/screen.py ./ztop

The pty is 100x30; every key is followed by a snapshot of the screen as
the escapes would leave it.  No termios is touched, so the program sees a
plain pty and uses its own raw mode.
"""
import fcntl
import os
import pty
import select
import struct
import sys
import termios
import time

W, H = 100, 30
grid = [[' '] * W for _ in range(H)]
row = col = 0
# G0 set: DEC Special Graphics letters when ESC(0 selected it, else ASCII.
# The library draws its frame that way; paint what the terminal would show.
acs = False
pending = b''
DEC = {'l': '\u250c', 'k': '\u2510', 'm': '\u2514', 'j': '\u2518',
       'q': '\u2500', 'x': '\u2502', 't': '\u251c', 'u': '\u2524',
       'w': '\u252c', 'v': '\u2534', 'n': '\u253c', 'a': '\u2592'}

def feed(data):
    global row, col, acs, pending
    data = pending + data
    pending = b''
    i = 0
    while i < len(data):
        b = data[i]
        if b == 0x1b and i + 1 >= len(data):
            pending = data[i:]
            return
        if b == 0x1b and data[i + 1] == ord('('):
            if i + 3 > len(data):
                pending = data[i:]
                return
            acs = data[i + 2] == ord('0')
            i += 3
            continue
        if b == 0x1b and data[i + 1] == ord('['):
            j = i + 2
            while j < len(data) and not (0x40 <= data[j] <= 0x7e):
                j += 1
            if j >= len(data):
                pending = data[i:]
                return
            params = data[i + 2:j].decode('latin1')
            final = chr(data[j])
            if final == 'H':
                parts = (params.split(';') + ['1', '1'])[:2]
                row = max(0, int(parts[0] or 1) - 1)
                col = max(0, int(parts[1] or 1) - 1)
            elif final == 'J':
                for r in range(H):
                    grid[r] = [' '] * W
            elif final == 'K':
                for c in range(col, W):
                    grid[row][c] = ' '
            i = j + 1
            continue
        if b == 0x0d:
            col = 0
        elif b == 0x0a:
            row = min(H - 1, row + 1)
        elif b >= 0x20:
            if row < H and col < W:
                c = chr(b)
                grid[row][col] = DEC.get(c, c) if acs else c
            col += 1
        i += 1

pid, fd = pty.fork()
if pid == 0:
    os.environ['TERM'] = 'xterm'
    prog = sys.argv[1] if len(sys.argv) > 1 else './ztop'
    os.execv(prog, [prog, '-d', '60'])  # noqa: S606 - exec is the point

fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack('HHHH', H, W, 0, 0))

def pump(t):
    end = time.time() + t
    while time.time() < end:
        r, _, _ = select.select([fd], [], [], 0.05)
        if not r:
            continue
        try:
            d = os.read(fd, 65536)
        except OSError:
            return
        if not d:
            return
        feed(d)

def show(what):
    print('===', what)
    for line in grid:
        print(''.join(line).rstrip())

pump(2.0)
show('initial')
for k in [b'\t', b'\t', b'\t', b'\t', b'?', b'?', b'q']:
    os.write(fd, k)
    pump(1.0)
    show(f'after {k!r}')
