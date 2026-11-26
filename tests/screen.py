#!/usr/bin/env python3
"""Paint ztop into a grid and print it, to check the layout without an eye.

    python3 tests/screen.py ./ztop

The pty is 100x30; every key is followed by a snapshot of the screen as
the escapes would leave it.  No termios is touched, so the program sees a
plain pty and uses its own raw mode.
"""
import fcntl, os, pty, select, struct, termios, time

W, H = 100, 30
grid = [[' '] * W for _ in range(H)]
row = col = 0

def feed(data):
    global row, col
    i = 0
    while i < len(data):
        b = data[i]
        if b == 0x1b and i + 1 < len(data) and data[i + 1] == ord('['):
            j = i + 2
            while j < len(data) and not (0x40 <= data[j] <= 0x7e):
                j += 1
            if j >= len(data):
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
                grid[row][col] = chr(b)
            col += 1
        i += 1

pid, fd = pty.fork()
if pid == 0:
    os.environ['TERM'] = 'xterm'
    os.execv('./ztop', ['./ztop', '-d', '60'])

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

pump(2.0)
for k in [b'1', b'2', b'4', b'\t', b'?', b'?', b'q']:
    os.write(fd, k)
    pump(1.0)
    print('=== after', k)
    for line in grid:
        print(''.join(line).rstrip())
