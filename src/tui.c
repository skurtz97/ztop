/**
 * @file tui.c
 * @brief See tui.h.  One output buffer per frame, one diff against the last.
 */
#include "tui.h"

#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <unistd.h>

#ifdef __MVS__
#define ESCS "\x27"      /* EBCDIC ESC; the tty converts it to 0x1b */
#else
#define ESCS "\x1b"
#endif

/* Input is not converted: keys and cursor reports arrive EBCDIC even in
 * raw mode, so every read is translated before it is looked at. */
#define ESC_CHAR 0x1bu

static void to_ascii(unsigned char *b, size_t n)
{
#ifdef __MVS__
    __e2a_l((char *)b, n);
#else
    (void)b;
    (void)n;
#endif
}

#define ALT_ON ESCS "[?1049h"
#define ALT_OFF ESCS "[?1049l"
#define CUR_OFF ESCS "[?25l"
#define CUR_ON ESCS "[?25h"
#define CLEAR ESCS "[H" ESCS "[2J"
#define RESET ESCS "[0m"

static struct tui *g_tui;
static volatile sig_atomic_t g_winch;

static void wr(int fd, const void *buf, size_t n)
{
    const char *p = buf;

    while (n > 0) {
        ssize_t k = write(fd, p, n);
        if (k < 0 && errno == EINTR)
            continue;
        if (k <= 0)
            return;
        p += (size_t)k;
        n -= (size_t)k;
    }
}

static void restore(struct tui *t)
{
    if (!t->open)
        return;
    wr(t->fd_out, RESET CUR_ON ALT_OFF, strlen(RESET CUR_ON ALT_OFF));
    (void)tcsetattr(t->fd_in, TCSANOW, &t->orig);
}

static void on_winch(int sig)
{
    (void)sig;
    g_winch = 1;
}

static void on_term(int sig)
{
    if (g_tui != NULL)
        restore(g_tui);
    _exit(128 + sig);
}

static void out_flush(struct tui *t)
{
    if (t->olen > 0) {
        wr(t->fd_out, t->out, (size_t)t->olen);
        t->olen = 0;
    }
}

static void out(struct tui *t, const char *s, size_t n)
{
    if (t->olen + (int)n > (int)sizeof t->out)
        out_flush(t);
    if (n > sizeof t->out) {
        wr(t->fd_out, s, n);
        return;
    }
    memcpy(t->out + t->olen, s, n);
    t->olen += (int)n;
}

static void outs(struct tui *t, const char *s)
{
    out(t, s, strlen(s));
}

static void outf(struct tui *t, const char *fmt, ...)
{
    char b[64];
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    if (n > 0)
        out(t, b, (size_t)n < sizeof b ? (size_t)n : sizeof b - 1);
}

static void sgr(struct tui *t, int fg, int bg, int attr)
{
    char b[40];
    int n = snprintf(b, sizeof b, "%s[0", ESCS);

    if (attr & TUI_BOLD)
        n += snprintf(b + n, sizeof b - (size_t)n, ";1");
    if (attr & TUI_DIM)
        n += snprintf(b + n, sizeof b - (size_t)n, ";2");
    if (attr & TUI_UNDERLINE)
        n += snprintf(b + n, sizeof b - (size_t)n, ";4");
    if (attr & TUI_REVERSE)
        n += snprintf(b + n, sizeof b - (size_t)n, ";7");
    n += snprintf(b + n, sizeof b - (size_t)n, ";3%d;4%dm", fg, bg);
    out(t, b, (size_t)n);
}

/* Ask the terminal where the cursor is; its answer is the only size z/OS
 * gives us.  Returns false if the terminal does not answer. */
static bool cpr_size(int in, int outfd, int *w, int *h)
{
    unsigned char b[64];
    int n = 0;
    bool got = false, esc = false, csi = false;
    int vals[2] = { 0, 0 }, nv = 0, v = 0;

    wr(outfd, ESCS "[6n", sizeof ESCS "[6n" - 1);
    for (int rounds = 0; rounds < 30 && !got; rounds++) {
        fd_set f;
        struct timeval tv = { 0, 10000 };

        FD_ZERO(&f);
        FD_SET(in, &f);
        int r = select(in + 1, &f, NULL, NULL, &tv);
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0)
            continue;
        ssize_t k = read(in, b + n, sizeof b - (size_t)n);
        if (k <= 0)
            continue;
        to_ascii(b + n, (size_t)k);
        for (ssize_t i = 0; i < k; i++) {
            unsigned char c = b[n + (size_t)i];
            if (!esc) {
                if (c == ESC_CHAR)
                    esc = true;
                continue;
            }
            if (!csi) {
                if (c == '[')
                    csi = true;
                else if (c != ESC_CHAR)
                    esc = false;
                continue;
            }
            if (c >= '0' && c <= '9') {
                v = v * 10 + (c - '0');
            } else if (c == ';' && nv == 0) {
                vals[nv++] = v;
                v = 0;
            } else if (c == 'R') {
                vals[nv++] = v;
                got = true;
                break;
            } else {
                esc = csi = false;
                nv = v = 0;
            }
        }
        n = 0;
    }
    if (!got || nv < 2 || vals[0] <= 0 || vals[1] <= 0)
        return false;
    *h = vals[0];
    *w = vals[1];
    return true;
}

static void probe_size(struct tui *t, int *w, int *h)
{
    struct winsize ws;

    *w = 80;
    *h = 24;
    if (ioctl(t->fd_out, TIOCGWINSZ, &ws) == 0 &&
        ws.ws_col > 0 && ws.ws_row > 0) {
        *w = ws.ws_col;
        *h = ws.ws_row;
    } else if (!cpr_size(t->fd_in, t->fd_out, w, h)) {
        const char *c = getenv("COLUMNS");
        const char *l = getenv("LINES");
        if (c != NULL && atoi(c) > 0)
            *w = atoi(c);
        if (l != NULL && atoi(l) > 0)
            *h = atoi(l);
    }
    if (*w > TUI_MAX_W)
        *w = TUI_MAX_W;
    if (*h > TUI_MAX_H)
        *h = TUI_MAX_H;
    if (*w < 20)
        *w = 80;
    if (*h < 5)
        *h = 24;
}

bool tui_resize(struct tui *t)
{
    int w, h;

    probe_size(t, &w, &h);
    if (w == t->w && h == t->h)
        return false;

    struct tui_cell *cur = calloc((size_t)w * (size_t)h, sizeof *cur);
    struct tui_cell *old = calloc((size_t)w * (size_t)h, sizeof *old);
    if (cur == NULL || old == NULL) {
        free(cur);
        free(old);
        return false;
    }
    free(t->cur);
    free(t->old);
    t->cur = cur;
    t->old = old;
    t->w = w;
    t->h = h;
    t->full = true;
    tui_clear(t);
    return true;
}

bool tui_open(struct tui *t)
{
    struct termios r;

    memset(t, 0, sizeof *t);
    t->fd_in = 0;
    t->fd_out = 1;
    if (!isatty(t->fd_in) || !isatty(t->fd_out))
        return false;
    if (tcgetattr(t->fd_in, &t->orig) != 0)
        return false;
    r = t->orig;
    r.c_lflag &= (tcflag_t)~(ICANON | ECHO | ISIG | IEXTEN);
    r.c_iflag &= (tcflag_t)~(IXON | ICRNL | BRKINT | INPCK | ISTRIP);
    r.c_oflag &= (tcflag_t)~OPOST;
    r.c_cc[VMIN] = 0;
    r.c_cc[VTIME] = 0;
    if (tcsetattr(t->fd_in, TCSANOW, &r) != 0)
        return false;

    t->open = true;
    g_tui = t;
    g_winch = 0;
    (void)signal(SIGTERM, on_term);
    (void)signal(SIGINT, on_term);
    (void)signal(SIGHUP, on_term);
    (void)signal(SIGQUIT, on_term);
#ifdef SIGWINCH
    (void)signal(SIGWINCH, on_winch);
#endif
    outs(t, ALT_ON CUR_OFF CLEAR);
    out_flush(t);
    if (!tui_resize(t)) {
        restore(t);
        t->open = false;
        g_tui = NULL;
        return false;
    }
    return true;
}

void tui_close(struct tui *t)
{
    if (!t->open)
        return;
    restore(t);
    t->open = false;
    g_tui = NULL;
    free(t->cur);
    free(t->old);
    t->cur = NULL;
    t->old = NULL;
}

void tui_size(struct tui *t, int *w, int *h)
{
    *w = t->w;
    *h = t->h;
}

void tui_clear(struct tui *t)
{
    int n = t->w * t->h;

    for (int i = 0; i < n; i++) {
        t->cur[i].ch = ' ';
        t->cur[i].fg = 7;
        t->cur[i].bg = 0;
        t->cur[i].attr = 0;
    }
}

void tui_fill(struct tui *t, int x, int y, int w, int h, char ch, tui_style st)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > t->w) w = t->w - x;
    if (y + h > t->h) h = t->h - y;
    if (w <= 0 || h <= 0)
        return;
    for (int j = y; j < y + h; j++) {
        struct tui_cell *row = t->cur + (size_t)j * (size_t)t->w;
        for (int i = x; i < x + w; i++) {
            row[i].ch = (unsigned char)ch;
            row[i].fg = st.fg;
            row[i].bg = st.bg;
            row[i].attr = st.attr;
        }
    }
}

void tui_put(struct tui *t, int x, int y, const char *s, int maxw, tui_style st)
{
    if (y < 0 || y >= t->h)
        return;
    struct tui_cell *row = t->cur + (size_t)y * (size_t)t->w;
    for (int i = 0; s[i] != '\0' && x < t->w; i++, x++) {
        unsigned char c = (unsigned char)s[i];
        if (maxw > 0 && i >= maxw)
            break;
        if (x < 0)
            continue;
        if (c < 0x20)
            c = '?';
        row[x].ch = c;
        row[x].fg = st.fg;
        row[x].bg = st.bg;
        row[x].attr = st.attr;
    }
}

void tui_printf(struct tui *t, int x, int y, int maxw, tui_style st,
                const char *fmt, ...)
{
    char b[512];
    va_list ap;

    va_start(ap, fmt);
    (void)vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    tui_put(t, x, y, b, maxw, st);
}

void tui_box(struct tui *t, int x, int y, int w, int h, tui_style st)
{
    if (w < 2 || h < 2)
        return;
    for (int i = 1; i < w - 1; i++) {
        tui_put(t, x + i, y, "-", 1, st);
        tui_put(t, x + i, y + h - 1, "-", 1, st);
    }
    for (int j = 1; j < h - 1; j++) {
        tui_put(t, x, y + j, "|", 1, st);
        tui_put(t, x + w - 1, y + j, "|", 1, st);
    }
    tui_put(t, x, y, "+", 1, st);
    tui_put(t, x + w - 1, y, "+", 1, st);
    tui_put(t, x, y + h - 1, "+", 1, st);
    tui_put(t, x + w - 1, y + h - 1, "+", 1, st);
}

void tui_flush(struct tui *t)
{
    int lfg = -1, lbg = -1, la = -1;

    if (!t->open)
        return;
    if (t->full) {
        outs(t, CLEAR);
        memset(t->old, 0, (size_t)t->w * (size_t)t->h * sizeof *t->old);
        t->full = false;
    }
    for (int y = 0; y < t->h; y++) {
        struct tui_cell *row = t->cur + (size_t)y * (size_t)t->w;
        struct tui_cell *orow = t->old + (size_t)y * (size_t)t->w;
        int x = 0;

        while (x < t->w) {
            while (x < t->w && row[x].ch == orow[x].ch &&
                   row[x].fg == orow[x].fg && row[x].bg == orow[x].bg &&
                   row[x].attr == orow[x].attr)
                x++;
            if (x >= t->w)
                break;
            int start = x;
            while (x < t->w && !(row[x].ch == orow[x].ch &&
                                 row[x].fg == orow[x].fg &&
                                 row[x].bg == orow[x].bg &&
                                 row[x].attr == orow[x].attr))
                x++;
            outf(t, "%s[%d;%dH", ESCS, y + 1, start + 1);
            for (int i = start; i < x; i++) {
                if (row[i].fg != lfg || row[i].bg != lbg || row[i].attr != la) {
                    sgr(t, row[i].fg, row[i].bg, row[i].attr);
                    lfg = row[i].fg;
                    lbg = row[i].bg;
                    la = row[i].attr;
                }
                char c = (char)(row[i].ch != 0 ? row[i].ch : ' ');
                out(t, &c, 1);
            }
        }
    }
    memcpy(t->old, t->cur, (size_t)t->w * (size_t)t->h * sizeof *t->cur);
    out_flush(t);
}

static void shift_in(struct tui *t, int n)
{
    if (n >= t->inlen) {
        t->inlen = 0;
        return;
    }
    memmove(t->in, t->in + n, (size_t)(t->inlen - n));
    t->inlen -= n;
}

static int decode(struct tui *t)
{
    unsigned char *b = t->in;

    if (t->inlen == 0)
        return TUI_KEY_NONE;
    if (b[0] != ESC_CHAR) {
        int k = b[0];
        shift_in(t, 1);
        return k;
    }
    if (t->inlen < 2)
        return TUI_KEY_NONE;
    if (b[1] != '[') {
        shift_in(t, 1);
        return TUI_KEY_ESC;
    }
    if (t->inlen < 3)
        return TUI_KEY_NONE;

    int k = TUI_KEY_NONE;
    switch (b[2]) {
    case 'A': k = TUI_KEY_UP; break;
    case 'B': k = TUI_KEY_DOWN; break;
    case 'C': k = TUI_KEY_RIGHT; break;
    case 'D': k = TUI_KEY_LEFT; break;
    case 'H': k = TUI_KEY_HOME; break;
    case 'F': k = TUI_KEY_END; break;
    default: break;
    }
    if (k != TUI_KEY_NONE) {
        shift_in(t, 3);
        return k;
    }
    if (b[2] >= '1' && b[2] <= '9') {
        int v = 0, i = 2;
        while (i < t->inlen && b[i] >= '0' && b[i] <= '9') {
            v = v * 10 + (b[i] - '0');
            i++;
        }
        if (i >= t->inlen)
            return TUI_KEY_NONE;      /* wait for the final byte */
        if (i < t->inlen && b[i] == '~') {
            switch (v) {
            case 1: case 7: k = TUI_KEY_HOME; break;
            case 2: k = TUI_KEY_NONE; break;
            case 3: k = TUI_KEY_DEL; break;
            case 4: case 8: k = TUI_KEY_END; break;
            case 5: k = TUI_KEY_PGUP; break;
            case 6: k = TUI_KEY_PGDN; break;
            default: break;
            }
            shift_in(t, i + 1);
            return k;
        }
    }
    /* Unknown CSI: drop the introducer and try again, so a paste of
     * unsupported sequences does not turn into random commands. */
    shift_in(t, 3);
    return TUI_KEY_NONE;
}

int tui_key(struct tui *t, int timeout_ms)
{
    if (g_winch) {
        g_winch = 0;
        return TUI_KEY_RESIZE;
    }
    for (;;) {
        if (t->inlen > 0) {
            int k = decode(t);
            if (k != TUI_KEY_NONE)
                return k;
        }
        if (g_winch) {
            g_winch = 0;
            return TUI_KEY_RESIZE;
        }
        int wait = (t->inlen > 0) ? 30 : timeout_ms;
        fd_set f;
        struct timeval tv, *ptv = NULL;

        FD_ZERO(&f);
        FD_SET(t->fd_in, &f);
        if (wait >= 0) {
            tv.tv_sec = wait / 1000;
            tv.tv_usec = (wait % 1000) * 1000;
            ptv = &tv;
        }
        int r = select(t->fd_in + 1, &f, NULL, NULL, ptv);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            return TUI_KEY_NONE;
        }
        if (r == 0) {
            if (t->inlen > 0) {
                if (t->inlen == 1 && t->in[0] == ESC_CHAR) {
                    t->inlen = 0;
                    return TUI_KEY_ESC;
                }
                t->inlen = 0;
            }
            return TUI_KEY_NONE;
        }
        ssize_t n = read(t->fd_in, t->in + t->inlen,
                         sizeof t->in - (size_t)t->inlen);
        if (n <= 0) {
            if (n < 0 && errno == EINTR)
                continue;
            return TUI_KEY_ESC;
        }
        to_ascii(t->in + t->inlen, (size_t)n);
        t->inlen += (int)n;
    }
}
