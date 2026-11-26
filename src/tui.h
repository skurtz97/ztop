/**
 * @file tui.h
 * @brief A small cell-based terminal UI: double buffer, diff flush, raw keys.
 *
 * Deliberately no ncurses and no terminfo.  Output is plain ANSI (xterm and
 * anything that copies it), input is decoded by hand.  Only bytes are stored,
 * so a cell is one character wide: ASCII art, not UTF-8 box drawing.  On z/OS
 * the C compiler's EBCDIC literals are converted by the tty, which is why
 * TUI_ESC below is 0x27 there and 0x1b everywhere else.
 */
#ifndef ZTOP_TUI_H
#define ZTOP_TUI_H

#include <stdbool.h>
#include <termios.h>

struct tui_cell {
    unsigned char ch;
    unsigned char fg;
    unsigned char bg;
    unsigned char attr;
};

typedef struct {
    unsigned char fg;   /**< 0-7, 7 is white */
    unsigned char bg;   /**< 0-7, 0 is black */
    unsigned char attr; /**< TUI_BOLD / TUI_REVERSE / TUI_DIM / TUI_UNDERLINE */
} tui_style;

#define TUI_BOLD 1u
#define TUI_REVERSE 2u
#define TUI_DIM 4u
#define TUI_UNDERLINE 8u

#define TUI_STYLE(f, b, a) ((tui_style){ (unsigned char)(f), (unsigned char)(b), (unsigned char)(a) })
#define TUI_PLAIN TUI_STYLE(7, 0, 0)
#define TUI_TITLE TUI_STYLE(7, 4, TUI_BOLD)
#define TUI_LABEL TUI_STYLE(6, 0, 0)
#define TUI_GOOD  TUI_STYLE(2, 0, 0)
#define TUI_WARN  TUI_STYLE(3, 0, 0)
#define TUI_BAD   TUI_STYLE(1, 0, TUI_BOLD)

enum {
    TUI_KEY_NONE = -1,
    TUI_KEY_ESC = 256,
    TUI_KEY_UP,
    TUI_KEY_DOWN,
    TUI_KEY_LEFT,
    TUI_KEY_RIGHT,
    TUI_KEY_HOME,
    TUI_KEY_END,
    TUI_KEY_PGUP,
    TUI_KEY_PGDN,
    TUI_KEY_DEL,
    TUI_KEY_RESIZE
};

#define TUI_MAX_W 500
#define TUI_MAX_H 200

struct tui {
    int w, h;
    int fd_in, fd_out;
    struct tui_cell *cur;
    struct tui_cell *old;
    bool full;
    bool open;
    struct termios orig;
    unsigned char in[64];
    int inlen;
    char out[32768];
    int olen;
};

bool tui_open(struct tui *t);
void tui_close(struct tui *t);

/** @brief Re-query the size; returns true when it changed and the buffers moved. */
bool tui_resize(struct tui *t);
void tui_size(struct tui *t, int *w, int *h);

void tui_clear(struct tui *t);
void tui_fill(struct tui *t, int x, int y, int w, int h, char ch, tui_style st);
void tui_put(struct tui *t, int x, int y, const char *s, int maxw, tui_style st);
void tui_printf(struct tui *t, int x, int y, int maxw, tui_style st,
                const char *fmt, ...);
void tui_box(struct tui *t, int x, int y, int w, int h, tui_style st);
void tui_flush(struct tui *t);

/** @brief Wait up to @p timeout_ms (-1 = forever) for a key. */
int tui_key(struct tui *t, int timeout_ms);

#endif
