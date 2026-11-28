/**
 * @file tui.c
 * @brief See tui.h.  One output buffer per frame, one diff against the last.
 */
#include "tui.h"

#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <time.h>
#include <unistd.h>

#ifdef __MVS__
#define ESCS "\x27"    /* EBCDIC ESC; the tty converts it to 0x1b */
#define ESC_CHAR 0x27u /* what a key press of ESC looks like here */
#else
#define ESCS "\x1b"
#define ESC_CHAR 0x1bu
#endif

#define ALT_ON ESCS "[?1049h"
#define ALT_OFF ESCS "[?1049l"
/* 1000 reports button presses and releases, 1006 gives SGR coordinates
 * (1-based, no 223 limit) so clicks beyond column 223 still work. */
#define MOUSE_ON ESCS "[?1000h" ESCS "[?1006h"
#define MOUSE_OFF ESCS "[?1006l" ESCS "[?1000l"
#define CUR_OFF ESCS "[?25l"
#define CUR_ON ESCS "[?25h"
#define CLEAR ESCS "[H" ESCS "[2J"
#define RESET ESCS "[0m"

static tui *g_tui;
static volatile sig_atomic_t g_winch;

static void wr(int fd, const void *buf, size_t n) {
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

static void restore(tui *t) {
    if (!t->open)
        return;
    /* Leave the graphics character set even if a signal cut a frame short. */
    wr(t->fd_out, RESET ESCS "(B" CUR_ON MOUSE_OFF ALT_OFF,
       strlen(RESET ESCS "(B" CUR_ON MOUSE_OFF ALT_OFF));
    (void)tcsetattr(t->fd_in, TCSANOW, &t->orig);
}

static void on_winch(int sig) {
    (void)sig;
    g_winch = 1;
}

static void on_term(int sig) {
    if (g_tui != nullptr)
        restore(g_tui);
    _exit(128 + sig);
}

static void out_flush(tui *t) {
    if (t->olen > 0) {
        wr(t->fd_out, t->out, (size_t)t->olen);
        t->olen = 0;
    }
}

static void out(tui *t, const char *s, size_t n) {
    if (t->olen + (int)n > (int)sizeof t->out)
        out_flush(t);
    if (n > sizeof t->out) {
        wr(t->fd_out, s, n);
        return;
    }
    memcpy(t->out + t->olen, s, n);
    t->olen += (int)n;
}

static void outs(tui *t, const char *s) {
    out(t, s, strlen(s));
}

static void outf(tui *t, const char *fmt, ...) {
    char b[64];
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    if (n > 0)
        out(t, b, (size_t)n < sizeof b ? (size_t)n : sizeof b - 1);
}

static void sgr(tui *t, int fg, int bg, int attr) {
    char b[64];
    int n = snprintf(b, sizeof b, "%s[0", ESCS);

    if (attr & TUI_BOLD)
        n += snprintf(b + n, sizeof b - (size_t)n, ";1");
    if (attr & TUI_DIM)
        n += snprintf(b + n, sizeof b - (size_t)n, ";2");
    if (attr & TUI_UNDERLINE)
        n += snprintf(b + n, sizeof b - (size_t)n, ";4");
    if (attr & TUI_REVERSE)
        n += snprintf(b + n, sizeof b - (size_t)n, ";7");
    if (fg == TUI_COLOR_DEFAULT)
        n += snprintf(b + n, sizeof b - (size_t)n, ";39");
    else
        n += snprintf(b + n, sizeof b - (size_t)n, ";38;5;%d", fg);
    if (bg == TUI_COLOR_DEFAULT)
        n += snprintf(b + n, sizeof b - (size_t)n, ";49m");
    else
        n += snprintf(b + n, sizeof b - (size_t)n, ";48;5;%dm", bg);
    out(t, b, (size_t)n);
}

/* Ask the terminal where the cursor is; its answer is the only size z/OS
 * gives us.  Returns false if the terminal does not answer. */
static bool cpr_size(int in, int outfd, int *w, int *h) {
    unsigned char b[64];
    int n = 0;
    bool got = false, esc = false, csi = false;
    int vals[2] = {0, 0}, nv = 0, v = 0;

    wr(outfd, ESCS "[6n", sizeof ESCS "[6n" - 1);
    for (int rounds = 0; rounds < 30 && !got; rounds++) {
        fd_set f;
        struct timeval tv = {0, 10000};

        FD_ZERO(&f);
        FD_SET(in, &f);
        int r = select(in + 1, &f, nullptr, nullptr, &tv);
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0)
            continue;
        ssize_t k = read(in, b + n, sizeof b - (size_t)n);
        if (k <= 0)
            continue;
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

static void probe_size(tui *t, int *w, int *h) {
    struct winsize ws;

    *w = 80;
    *h = 24;
    if (ioctl(t->fd_out, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0 &&
        ws.ws_row > 0) {
        *w = ws.ws_col;
        *h = ws.ws_row;
    } else if (!cpr_size(t->fd_in, t->fd_out, w, h)) {
        const char *c = getenv("COLUMNS");
        const char *l = getenv("LINES");
        if (c != nullptr && atoi(c) > 0)
            *w = atoi(c);
        if (l != nullptr && atoi(l) > 0)
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

bool tui_alloc(tui *t, int w, int h) {
    tui_cell *cur, *old;

    if (w < 2)
        w = 2;
    if (h < 2)
        h = 2;
    if (w > TUI_MAX_W)
        w = TUI_MAX_W;
    if (h > TUI_MAX_H)
        h = TUI_MAX_H;
    cur = calloc((size_t)w * (size_t)h, sizeof *cur);
    old = calloc((size_t)w * (size_t)h, sizeof *old);
    if (cur == nullptr || old == nullptr) {
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

bool tui_resize(tui *t) {
    int w, h;

    probe_size(t, &w, &h);
    if (w == t->w && h == t->h)
        return false;
    return tui_alloc(t, w, h);
}

/* z/OS C runtime prefers sigaction(); plain signal() is not used. */
static void install_signal(int sig, void (*fn)(int)) {
    struct sigaction sa = {};

    sa.sa_handler = fn;
    (void)sigemptyset(&sa.sa_mask);
    (void)sigaction(sig, &sa, nullptr);
}

/* DEC Special Graphics are VT100 core, but a terminal we do not recognise
 * is not worth the risk: `lqqqk` where a corner should be is worse than
 * `+----+`.  This is the one place the glyph backend is chosen. */
static bool term_glyphs(void) {
    static const char *const ok[] = {
        "xterm", "screen",  "tmux",   "rxvt",    "vt100", "vt220",
        "linux", "ansi",    "cygwin", "alacrit", "foot",  "wezterm",
        "kitty", "konsole", "putty",  "gnome",   "iterm", "st",
    };
    const char *term = getenv("TERM");

    if (term == nullptr || term[0] == '\0')
        return false;
    for (size_t i = 0; i < sizeof ok / sizeof ok[0]; i++)
        if (strncmp(term, ok[i], strlen(ok[i])) == 0)
            return true;
    return false;
}

bool tui_open(tui *t) {
    struct termios r;

    memset(t, 0, sizeof *t);
    t->glyphs = term_glyphs();
    t->fd_in = 0;
    t->fd_out = 1;
    if (!isatty(t->fd_in) || !isatty(t->fd_out))
        return false;
    if (tcgetattr(t->fd_in, &t->orig) != 0)
        return false;
    r = t->orig;
    r.c_lflag &= (tcflag_t) ~(ICANON | ECHO | ISIG | IEXTEN);
    r.c_iflag &= (tcflag_t) ~(IXON | ICRNL | BRKINT | INPCK | ISTRIP);
    r.c_oflag &= (tcflag_t)~OPOST;
    r.c_cc[VMIN] = 0;
    r.c_cc[VTIME] = 0;
    if (tcsetattr(t->fd_in, TCSANOW, &r) != 0)
        return false;

    t->open = true;
    g_tui = t;
    g_winch = 0;
    install_signal(SIGTERM, on_term);
    install_signal(SIGINT, on_term);
    install_signal(SIGHUP, on_term);
    install_signal(SIGQUIT, on_term);
#ifdef SIGWINCH
    install_signal(SIGWINCH, on_winch);
#endif
    outs(t, ALT_ON CUR_OFF CLEAR MOUSE_ON);
    out_flush(t);
    if (!tui_resize(t)) {
        restore(t);
        t->open = false;
        g_tui = nullptr;
        return false;
    }
    return true;
}

void tui_close(tui *t) {
    if (!t->open)
        return;
    restore(t);
    t->open = false;
    g_tui = nullptr;
    free(t->cur);
    free(t->old);
    t->cur = nullptr;
    t->old = nullptr;
}

void tui_size(tui *t, int *w, int *h) {
    *w = t->w;
    *h = t->h;
}

void tui_clear(tui *t) {
    int n = t->w * t->h;

    for (int i = 0; i < n; i++) {
        t->cur[i].ch = ' ';
        t->cur[i].fg = TUI_PLAIN.fg;
        t->cur[i].bg = TUI_PLAIN.bg;
        t->cur[i].attr = 0;
    }
}

/* ---- layout primitives ----------------------------------------------- */

void tui_split_v(tui_rect r, int top_h, tui_rect *top, tui_rect *bot) {
    if (top_h < 0)
        top_h = 0;
    if (top_h > r.h)
        top_h = r.h;
    if (top != nullptr)
        *top = (tui_rect){r.x, r.y, r.w, top_h};
    if (bot != nullptr)
        *bot = (tui_rect){r.x, r.y + top_h, r.w, r.h - top_h};
}

void tui_split_h(tui_rect r, int left_w, tui_rect *left, tui_rect *right) {
    if (left_w < 0)
        left_w = 0;
    if (left_w > r.w)
        left_w = r.w;
    if (left != nullptr)
        *left = (tui_rect){r.x, r.y, left_w, r.h};
    if (right != nullptr)
        *right = (tui_rect){r.x + left_w, r.y, r.w - left_w, r.h};
}

tui_rect tui_inset(tui_rect r, int dx, int dy) {
    int nw = r.w - 2 * dx;
    int nh = r.h - 2 * dy;

    return (tui_rect){r.x + dx, r.y + dy, nw > 0 ? nw : 0, nh > 0 ? nh : 0};
}

bool tui_rect_hit(tui_rect r, int x, int y) {
    if (x < r.x || x >= r.x + r.w)
        return false;
    if (y < r.y || y >= r.y + r.h)
        return false;
    return true;
}

void tui_fill(tui *t, int x, int y, int w, int h, char ch, tui_style st) {
    if (x < 0) {
        w += x;
        x = 0;
    }
    if (y < 0) {
        h += y;
        y = 0;
    }
    if (x + w > t->w)
        w = t->w - x;
    if (y + h > t->h)
        h = t->h - y;
    if (w <= 0 || h <= 0)
        return;
    for (int j = y; j < y + h; j++) {
        tui_cell *row = t->cur + (size_t)j * (size_t)t->w;
        for (int i = x; i < x + w; i++) {
            row[i].ch = (unsigned char)ch;
            row[i].fg = st.fg;
            row[i].bg = st.bg;
            row[i].attr = st.attr;
        }
    }
}

void tui_put(tui *t, int x, int y, const char *s, int maxw, tui_style st) {
    if (y < 0 || y >= t->h)
        return;
    tui_cell *row = t->cur + (size_t)y * (size_t)t->w;
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

void tui_printf(tui *t, int x, int y, int maxw, tui_style st, const char *fmt,
                ...) {
    char b[512];
    va_list ap;

    va_start(ap, fmt);
    (void)vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    tui_put(t, x, y, b, maxw, st);
}

void tui_box(tui *t, int x, int y, int w, int h, tui_style st) {
    tui_box_frame(t, x, y, w, h, st, nullptr, TUI_PLAIN);
}

/* ---- line drawing ---------------------------------------------------- */

/* A cell holds one byte, so a line is either a DEC Special Graphics letter
 * (the terminal draws the line) or the ASCII the frame has always used.
 * The order is the order of the calls below and means nothing else. */
enum { G_TL, G_TR, G_BL, G_BR, G_H, G_V, G_SHADE, G_COUNT };

static char glyph(const tui *t, int g) {
    static const char dec[] = {'l', 'k', 'm', 'j', 'q', 'x', 'a'};
    static const char ascii[] = {'+', '+', '+', '+', '-', '|', '#'};

    return t->glyphs ? dec[g] : ascii[g];
}

static void putg(tui *t, int x, int y, int g, tui_style st) {
    char s[2] = {glyph(t, g), '\0'};

    if (t->glyphs)
        st.attr = (unsigned char)(st.attr | TUI_ACS);
    tui_put(t, x, y, s, 1, st);
}

void tui_box_frame(tui *t, int x, int y, int w, int h, tui_style st,
                   const char *title, tui_style title_st) {
    if (w < 2 || h < 2)
        return;
    for (int i = 1; i < w - 1; i++) {
        putg(t, x + i, y, G_H, st);
        putg(t, x + i, y + h - 1, G_H, st);
    }
    for (int j = 1; j < h - 1; j++) {
        putg(t, x, y + j, G_V, st);
        putg(t, x + w - 1, y + j, G_V, st);
    }
    putg(t, x, y, G_TL, st);
    putg(t, x + w - 1, y, G_TR, st);
    putg(t, x, y + h - 1, G_BL, st);
    putg(t, x + w - 1, y + h - 1, G_BR, st);
    if (title != nullptr && title[0] != '\0' && w > 6) {
        char chip[64];
        int n = (int)strlen(title);
        int room = w - 6;

        if (n > room)
            n = room;
        (void)snprintf(chip, sizeof chip, " %.*s ", n, title);
        tui_printf(t, x + 2, y, (int)strlen(chip), title_st, "%s", chip);
    }
}

void tui_flush(tui *t) {
    int lfg = -1, lbg = -1, la = -1;

    if (!t->open)
        return;
    if (t->full) {
        outs(t, CLEAR);
        memset(t->old, 0, (size_t)t->w * (size_t)t->h * sizeof *t->old);
        t->full = false;
    }
    for (int y = 0; y < t->h; y++) {
        tui_cell *row = t->cur + (size_t)y * (size_t)t->w;
        tui_cell *orow = t->old + (size_t)y * (size_t)t->w;
        int x = 0;

        while (x < t->w) {
            while (x < t->w && row[x].ch == orow[x].ch &&
                   row[x].fg == orow[x].fg && row[x].bg == orow[x].bg &&
                   row[x].attr == orow[x].attr)
                x++;
            if (x >= t->w)
                break;
            int start = x;
            while (x < t->w &&
                   !(row[x].ch == orow[x].ch && row[x].fg == orow[x].fg &&
                     row[x].bg == orow[x].bg && row[x].attr == orow[x].attr))
                x++;
            outf(t, "%s[%d;%dH", ESCS, y + 1, start + 1);
            for (int i = start; i < x; i++) {
                bool acs = (row[i].attr & TUI_ACS) != 0;

                if (row[i].fg != lfg || row[i].bg != lbg || row[i].attr != la) {
                    sgr(t, row[i].fg, row[i].bg, row[i].attr);
                    lfg = row[i].fg;
                    lbg = row[i].bg;
                    la = row[i].attr;
                }
                /* An SGR reset does not leave the graphics set, so this is
                 * tracked apart from the colours. */
                if (acs != t->acs_on) {
                    outs(t, acs ? ESCS "(0" : ESCS "(B");
                    t->acs_on = acs;
                }
                char c = (char)(row[i].ch != 0 ? row[i].ch : ' ');
                out(t, &c, 1);
            }
        }
    }
    memcpy(t->old, t->cur, (size_t)t->w * (size_t)t->h * sizeof *t->cur);
    out_flush(t);
}

int tui_tabs_hit(int x, const char *const *labels, int n) {
    int at = 0;

    for (int i = 0; i < n; i++) {
        int len = (int)strlen(labels[i]) + 2;

        if (x >= at && x < at + len)
            return i;
        at += len + 2; /* the gap drawn by tui_tabs_draw */
    }
    return -1;
}

int tui_tabs_draw(tui *t, int x, int y, const char *const *labels, int n,
                  int active) {
    for (int i = 0; i < n; i++) {
        int len = (int)strlen(labels[i]) + 2;

        tui_printf(t, x, y, len, i == active ? TUI_TAB_ON : TUI_TAB_OFF, " %s ",
                   labels[i]);
        x += len;
        if (i + 1 < n)
            x += 2; /* a gap, not a `|`: separators read as a DOS menu */
    }
    return x;
}

void tui_rule(tui *t, int y, char ch, tui_style st) {
    tui_fill(t, 0, y, t->w, 1, ch, st);
}

static void shift_in(tui *t, int n) {
    if (n >= t->inlen) {
        t->inlen = 0;
        return;
    }
    memmove(t->in, t->in + n, (size_t)(t->inlen - n));
    t->inlen -= n;
}

static int decode(tui *t) {
    unsigned char *b = t->in;

    if (t->inlen == 0)
        return TUI_KEY_NONE;
    if (b[0] != ESC_CHAR) {
        int k = b[0];
        shift_in(t, 1);
        return k == '\t' ? TUI_KEY_TAB : k;
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
    case 'A':
        k = TUI_KEY_UP;
        break;
    case 'B':
        k = TUI_KEY_DOWN;
        break;
    case 'C':
        k = TUI_KEY_RIGHT;
        break;
    case 'D':
        k = TUI_KEY_LEFT;
        break;
    case 'H':
        k = TUI_KEY_HOME;
        break;
    case 'F':
        k = TUI_KEY_END;
        break;
    case 'Z':
        k = TUI_KEY_BACKTAB;
        break;
    default:
        break;
    }
    if (k != TUI_KEY_NONE) {
        shift_in(t, 3);
        return k;
    }
    if (b[2] == '<') {
        int v[3] = {0, 0, 0};
        int n = 0;
        int i = 3;

        while (i < t->inlen && n < 3) {
            if (b[i] >= '0' && b[i] <= '9') {
                v[n] = v[n] * 10 + (b[i] - '0');
                i++;
            } else if (b[i] == ';') {
                n++;
                i++;
            } else {
                break;
            }
        }
        if (i >= t->inlen)
            return TUI_KEY_NONE; /* wait for the final byte */
        if (b[i] != 'M' && b[i] != 'm') {
            shift_in(t, i + 1);
            return TUI_KEY_NONE;
        }
        t->mouse.button = v[0];
        t->mouse.x = v[1] > 0 ? v[1] - 1 : 0;
        t->mouse.y = v[2] > 0 ? v[2] - 1 : 0;
        t->mouse.release = b[i] == 'm';
        shift_in(t, i + 1);
        return TUI_KEY_MOUSE;
    }
    if (b[2] >= '1' && b[2] <= '9') {
        int v = 0, i = 2;
        while (i < t->inlen && b[i] >= '0' && b[i] <= '9') {
            v = v * 10 + (b[i] - '0');
            i++;
        }
        if (i >= t->inlen)
            return TUI_KEY_NONE; /* wait for the final byte */
        if (i < t->inlen && b[i] == '~') {
            switch (v) {
            case 1:
            case 7:
                k = TUI_KEY_HOME;
                break;
            case 2:
                k = TUI_KEY_NONE;
                break;
            case 3:
                k = TUI_KEY_DEL;
                break;
            case 4:
            case 8:
                k = TUI_KEY_END;
                break;
            case 5:
                k = TUI_KEY_PGUP;
                break;
            case 6:
                k = TUI_KEY_PGDN;
                break;
            default:
                break;
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

int tui_key(tui *t, int timeout_ms) {
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
        struct timeval tv, *ptv = nullptr;

        FD_ZERO(&f);
        FD_SET(t->fd_in, &f);
        if (wait >= 0) {
            tv.tv_sec = wait / 1000;
            tv.tv_usec = (wait % 1000) * 1000;
            ptv = &tv;
        }
        int r = select(t->fd_in + 1, &f, nullptr, nullptr, ptv);
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
        ssize_t n =
            read(t->fd_in, t->in + t->inlen, sizeof t->in - (size_t)t->inlen);
        if (n <= 0) {
            if (n < 0 && errno == EINTR)
                continue;
            return TUI_KEY_ESC;
        }
        t->inlen += (int)n;
    }
}

/* ---- messages and the application loop ------------------------------- */

/** @brief Set @p due to now + @p ms on the monotonic clock. */
static void deadline(struct timespec *due, int ms) {
    (void)clock_gettime(CLOCK_MONOTONIC, due);
    due->tv_sec += ms / 1000;
    due->tv_nsec += (long)(ms % 1000) * 1000000L;
    if (due->tv_nsec >= 1000000000L) {
        due->tv_nsec -= 1000000000L;
        due->tv_sec++;
    }
}

/** @brief Milliseconds from now until @p due; 0 once it has passed. */
static int remaining(const struct timespec *due) {
    struct timespec now;
    long long ms;

    (void)clock_gettime(CLOCK_MONOTONIC, &now);
    ms = (long long)(due->tv_sec - now.tv_sec) * 1000 +
         (long)(due->tv_nsec - now.tv_nsec) / 1000000L;
    return ms < 0 ? 0 : (int)ms;
}

void tui_run(tui *t, tui_app *app) {
    struct timespec due;
    int tick = app->tick_ms;

    app->quit = false;
    app->view(app->model, t);
    tui_flush(t);
    if (tick > 0)
        deadline(&due, tick);
    for (;;) {
        /* The interval is a deadline, not a wait restarted by each key:
         * a held key would otherwise starve the tick. */
        int k = tui_key(t, tick > 0 ? remaining(&due) : -1);
        tui_msg m;

        m.key = 0;
        m.w = t->w;
        m.h = t->h;
        if (k == TUI_KEY_NONE) {
            if (tick <= 0)
                continue;
            m.type = TUI_MSG_TICK;
            deadline(&due, tick);
        } else if (k == TUI_KEY_RESIZE) {
            (void)tui_resize(t);
            m.type = TUI_MSG_RESIZE;
        } else if (k == TUI_KEY_MOUSE) {
            m.type = TUI_MSG_MOUSE;
            m.mouse = t->mouse;
        } else {
            m.type = TUI_MSG_KEY;
            m.key = k;
        }
        if (app->update(app, &m)) {
            app->view(app->model, t);
            tui_flush(t);
        }
        if (app->quit)
            break;
        if (app->tick_ms != tick) {
            tick = app->tick_ms;
            if (tick > 0)
                deadline(&due, tick);
        }
    }
}

/* ---- viewport -------------------------------------------------------- */

void tui_viewport_clamp(tui_viewport *v, int rows, int height) {
    if (rows < 0)
        rows = 0;
    if (height < 1)
        height = 1;
    if (v->sel < 0)
        v->sel = 0;
    if (v->sel >= rows)
        v->sel = rows > 0 ? rows - 1 : 0;
    if (v->sel < v->top)
        v->top = v->sel;
    if (v->sel >= v->top + height)
        v->top = v->sel - height + 1;
    if (v->top > rows - height)
        v->top = rows - height;
    if (v->top < 0)
        v->top = 0;
}

void tui_viewport_move(tui_viewport *v, int delta, int rows, int height) {
    v->sel += delta;
    tui_viewport_clamp(v, rows, height);
}

bool tui_viewport_key(tui_viewport *v, int key, int rows, int height) {
    switch (key) {
    case 'j':
    case TUI_KEY_DOWN:
        tui_viewport_move(v, 1, rows, height);
        return true;
    case 'k':
    case TUI_KEY_UP:
        tui_viewport_move(v, -1, rows, height);
        return true;
    case TUI_KEY_PGDN:
        tui_viewport_move(v, height, rows, height);
        return true;
    case TUI_KEY_PGUP:
        tui_viewport_move(v, -height, rows, height);
        return true;
    case 'g':
    case TUI_KEY_HOME:
        v->sel = 0;
        tui_viewport_clamp(v, rows, height);
        return true;
    case 'G':
    case TUI_KEY_END:
        v->sel = rows - 1;
        tui_viewport_clamp(v, rows, height);
        return true;
    default:
        return false;
    }
}

/* ---- table ----------------------------------------------------------- */

/* A proportional thumb on the last free column, only when there is more to
 * see than fits. */
static void draw_scrollbar(tui *t, tui_rect r, int rows, int top, int height) {
    int x = r.x + r.w;
    int thumb_h, max_top, thumb_y;

    if (rows <= height || r.w <= 2)
        return;
    if (x >= t->w)
        x = r.x + r.w - 1;
    thumb_h = height * height / rows;
    if (thumb_h < 1)
        thumb_h = 1;
    max_top = rows - height;
    thumb_y = max_top > 0 ? top * (height - thumb_h) / max_top : 0;
    for (int i = 0; i < height; i++) {
        int on = i >= thumb_y && i < thumb_y + thumb_h;

        putg(t, x, r.y + 1 + i, on != 0 ? G_SHADE : G_V,
             on != 0 ? TUI_ACCENT : TUI_META);
    }
}

void tui_table_draw(tui *t, tui_rect r, const tui_table *tb, int rows,
                    tui_cell_fn cell, tui_rowstyle_fn rowstyle, void *ctx) {
    int x = r.x;
    int height = r.h - 1;
    char buf[TUI_CELL_MAX];

    if (r.w <= 0 || r.h <= 0)
        return;
    tui_fill(t, r.x, r.y, r.w, 1, ' ', TUI_LABEL);
    for (int c = 0; c < tb->ncols && x < r.x + r.w; c++) {
        int w = tb->cols[c].width;
        char up[32];
        size_t n;

        if (w > r.x + r.w - x)
            w = r.x + r.w - x;
        n = strlen(tb->cols[c].title);
        if (n >= sizeof up)
            n = sizeof up - 1;
        for (size_t k = 0; k < n; k++)
            up[k] = (char)toupper((unsigned char)tb->cols[c].title[k]);
        up[n] = '\0';
        if (tb->cols[c].right)
            tui_printf(t, x, r.y, w, TUI_LABEL, "%*s", w, up);
        else
            tui_printf(t, x, r.y, w, TUI_LABEL, "%-*s", w, up);
        x += tb->cols[c].width;
        if (c + 1 < tb->ncols)
            x += 1;
    }
    if (height < 1)
        return;

    tui_viewport vp = tb->vp;
    tui_viewport_clamp(&vp, rows, height);
    for (int i = 0; i < height && vp.top + i < rows; i++) {
        int row = vp.top + i;
        int y = r.y + 1 + i;
        tui_style st = rowstyle != nullptr ? rowstyle(ctx, row) : TUI_PLAIN;
        bool cur = row == vp.sel;

        x = r.x;
        if (cur) {
            tui_style fs = st;

            fs.bg = TUI_SEL_BG;
            tui_fill(t, r.x, y, r.w, 1, ' ', fs);
            st = fs;
        } else {
            tui_fill(t, r.x, y, r.w, 1, ' ', TUI_PLAIN);
        }
        for (int c = 0; c < tb->ncols && x < r.x + r.w; c++) {
            int w = tb->cols[c].width;

            if (w > r.x + r.w - x)
                w = r.x + r.w - x;
            buf[0] = '\0';
            if (cell != nullptr)
                cell(ctx, row, c, buf, sizeof buf);
            if (tb->cols[c].right)
                tui_printf(t, x, y, w, st, "%*s", w, buf);
            else
                tui_printf(t, x, y, w, st, "%-*s", w, buf);
            x += tb->cols[c].width;
            if (c + 1 < tb->ncols)
                x += 1;
        }
    }
    draw_scrollbar(t, r, rows, vp.top, height);
}

bool tui_table_key(tui_table *tb, int key, int rows, int height) {
    if (tb->ncols == 0 && key == 0)
        return false;
    return tui_viewport_key(&tb->vp, key, rows, height);
}

/* ---- tabs ------------------------------------------------------------ */

bool tui_tabs_key(int key, int *active, int n) {
    switch (key) {
    case TUI_KEY_RIGHT:
    case TUI_KEY_TAB:
        *active = (*active + 1) % n;
        return true;
    case TUI_KEY_LEFT:
    case TUI_KEY_BACKTAB:
        *active = (*active + n - 1) % n;
        return true;
    default:
        return false;
    }
}

/* ---- text input ------------------------------------------------------ */

void tui_input_draw(tui *t, tui_rect r, const tui_input *in, tui_style st) {
    if (r.w < 3 || r.h < 1)
        return;
    tui_fill(t, r.x, r.y, r.w, 1, ' ', st);
    int w = r.w - 2;
    int start = 0;

    if (in->pos >= w)
        start = in->pos - w + 1;
    tui_printf(t, r.x + 1, r.y, w, st, "%.*s", w, in->buf + start);
    if (in->focus) {
        tui_style cur = TUI_STYLE(16, 252, 0);
        int cx = r.x + 1 + in->pos - start;

        if (cx < r.x + 1 + w) {
            char c = in->pos < in->len ? in->buf[in->pos] : ' ';
            tui_put(t, cx, r.y, &c, 1, cur);
        }
    }
}

bool tui_input_key(tui_input *in, int key) {
    if (!in->focus)
        return false;
    if (key >= 32 && key < 127) {
        if (in->len + 1 >= (int)sizeof in->buf)
            return true;
        memmove(in->buf + in->pos + 1, in->buf + in->pos,
                (size_t)(in->len - in->pos));
        in->buf[in->pos] = (char)key;
        in->len++;
        in->pos++;
        in->buf[in->len] = '\0';
        return true;
    }
    if (key == 8 || key == 127) {
        if (in->pos > 0) {
            memmove(in->buf + in->pos - 1, in->buf + in->pos,
                    (size_t)(in->len - in->pos));
            in->pos--;
            in->len--;
            in->buf[in->len] = '\0';
        }
        return true;
    }
    if (key == TUI_KEY_DEL) {
        if (in->pos < in->len) {
            memmove(in->buf + in->pos, in->buf + in->pos + 1,
                    (size_t)(in->len - in->pos - 1));
            in->len--;
            in->buf[in->len] = '\0';
        }
        return true;
    }
    if (key == TUI_KEY_LEFT && in->pos > 0) {
        in->pos--;
        return true;
    }
    if (key == TUI_KEY_RIGHT && in->pos < in->len) {
        in->pos++;
        return true;
    }
    if (key == TUI_KEY_HOME) {
        in->pos = 0;
        return true;
    }
    if (key == TUI_KEY_END) {
        in->pos = in->len;
        return true;
    }
    return false;
}

/* ---- progress and status -------------------------------------------- */

void tui_progress(tui *t, int x, int y, int width, int pct) {
    tui_style st;
    int fill, i;

    if (width < 2)
        return;
    fill = pct < 0 ? 0 : pct * width / 100;
    if (fill > width)
        fill = width;
    st = pct < 0    ? TUI_META
         : pct < 70 ? TUI_GOOD
         : pct < 85 ? TUI_WARN
                    : TUI_BAD;
    /* A track with a shaded fill, no brackets: `[####    ]` is the other
     * thing that read as curses. */
    for (i = 0; i < width; i++)
        putg(t, x + i, y, i < fill ? G_SHADE : G_H, i < fill ? st : TUI_META);
}

void tui_keymap_footer(tui *t, int x, int y, int maxw, const tui_binding *km,
                       int n, int tab) {
    unsigned bit = 1u << (unsigned)tab;

    for (int i = 0; i < n; i++) {
        int kl, ll;

        if (!km[i].in_footer || (km[i].tabs & bit) == 0)
            continue;
        kl = (int)strlen(km[i].key);
        ll = (int)strlen(km[i].label);
        if (x + kl + ll + 4 > maxw)
            break;
        tui_put(t, x, y, " ", 1, TUI_BAR);
        tui_printf(t, x + 1, y, kl, TUI_KEYCAP, "%s", km[i].key);
        tui_put(t, x + 1 + kl, y, " ", 1, TUI_BAR);
        tui_printf(t, x + 2 + kl, y, ll, TUI_BAR, "%s", km[i].label);
        x += kl + ll + 3;
    }
}

void tui_keymap_help(tui *t, const tui_binding *km, int n, const char *title) {
    int w = t->w < 68 ? t->w - 2 : 66;
    int h = t->h < 20 ? t->h - 2 : 18;
    int x = (t->w - w) / 2;
    int y = (t->h - h) / 2;
    const char *group = nullptr;
    int row = y + 2;
    int col_x = x + 2;

    if (x < 0)
        x = 0;
    if (y < 0)
        y = 0;
    tui_fill(t, x, y, w, h, ' ', TUI_PLAIN);
    tui_box_frame(t, x, y, w, h, TUI_ACCENT, title, TUI_ACCENT);
    for (int i = 0; i < n && row < y + h - 2; i++) {
        int kl, ll, need;

        if (group == nullptr || strcmp(group, km[i].group) != 0) {
            if (group != nullptr)
                row += 2;
            if (row >= y + h - 2)
                break;
            group = km[i].group;
            tui_printf(t, x + 2, row++, w - 4, TUI_LABEL, "%s", group);
            col_x = x + 2;
        }
        kl = (int)strlen(km[i].key);
        ll = (int)strlen(km[i].label);
        need = kl + ll + 4;
        if (col_x + need > x + w - 2) {
            row++;
            col_x = x + 2;
            if (row >= y + h - 2)
                break;
        }
        tui_printf(t, col_x, row, kl, TUI_KEYCAP, "%s", km[i].key);
        tui_printf(t, col_x + kl + 1, row, ll, TUI_META, "%s", km[i].label);
        col_x += need;
    }
    tui_printf(t, x + 2, y + h - 2, w - 4, TUI_META, "press ? or ESC to close");
}

void tui_sparkline(tui *t, int x, int y, const unsigned char *vals, int n,
                   tui_style st) {
    static const char ramp[] = " .:-=+*#%";

    for (int i = 0; i < n; i++) {
        unsigned v = vals[i] > 100u ? 100u : vals[i];
        unsigned idx = (v * 8u + 50u) / 100u;
        /* tui_put expects a terminated string, so the sample is one. */
        char c[2] = {ramp[idx], '\0'};

        tui_put(t, x + i, y, c, 1, st);
    }
}

void tui_statusbar(tui *t, const char *left, const char *right) {
    int y = t->h - 1;

    tui_fill(t, 0, y, t->w, 1, ' ', TUI_BAR);
    if (left != nullptr)
        tui_printf(t, 1, y, t->w - 2, TUI_PLAIN, "%s", left);
    if (right != nullptr) {
        int n = (int)strlen(right);
        tui_printf(t, t->w - 1 - n, y, n, TUI_PLAIN, "%s", right);
    }
}
