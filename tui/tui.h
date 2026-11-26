/**
 * @file tui.h
 * @brief A small terminal application toolkit: cell buffer, diff renderer,
 *        message loop, and a few widgets (viewport, table, tabs, input).
 *
 * Deliberately no ncurses and no terminfo.  Output is plain ANSI (xterm and
 * anything that copies it), input is decoded by hand.  The shape follows the
 * Elm architecture as popularised by Bubble Tea: an application owns a model,
 * receives messages (keys, resize, ticks), and draws from the model.  Only
 * bytes are stored, so a cell is one character wide: ASCII art, not UTF-8 box
 * drawing.  On z/OS the C compiler's EBCDIC literals are converted by the
 * tty, which is why TUI_ESC below is 0x27 there and 0x1b everywhere else.
 */
#ifndef ZTOP_TUI_H
#define ZTOP_TUI_H

#include <stdbool.h>
#include <stddef.h>
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

typedef struct {
    int x, y, w, h;
} tui_rect;

#define TUI_BOLD 1u
#define TUI_REVERSE 2u
#define TUI_DIM 4u
#define TUI_UNDERLINE 8u

#define TUI_STYLE(f, b, a)                                                     \
    ((tui_style){(unsigned char)(f), (unsigned char)(b), (unsigned char)(a)})
#define TUI_PLAIN TUI_STYLE(7, 0, 0)
#define TUI_TITLE TUI_STYLE(7, 4, TUI_BOLD)
#define TUI_LABEL TUI_STYLE(6, 0, 0)
#define TUI_GOOD TUI_STYLE(2, 0, 0)
#define TUI_WARN TUI_STYLE(3, 0, 0)
#define TUI_BAD TUI_STYLE(1, 0, TUI_BOLD)
#define TUI_TAB_ON TUI_STYLE(0, 7, TUI_BOLD)
#define TUI_TAB_OFF TUI_STYLE(7, 0, TUI_DIM)

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
    TUI_KEY_RESIZE,
    TUI_KEY_TAB,
    TUI_KEY_BACKTAB,
    TUI_KEY_MOUSE,
};

#define TUI_MAX_W 500
#define TUI_MAX_H 200

/** @brief One mouse event; coordinates are 0-based cells. */
struct tui_mouse {
    int x, y;
    int button; /**< 0 left, 1 middle, 2 right, 64 wheel up, 65 wheel down */
    bool release;
};

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
    struct tui_mouse mouse;
};

bool tui_alloc(struct tui *t, int w, int h);
bool tui_open(struct tui *t);
void tui_close(struct tui *t);

/** @brief Re-query the size; returns true when it changed and the buffers
 * moved. */
bool tui_resize(struct tui *t);
void tui_size(struct tui *t, int *w, int *h);

void tui_clear(struct tui *t);
void tui_fill(struct tui *t, int x, int y, int w, int h, char ch, tui_style st);
void tui_put(struct tui *t, int x, int y, const char *s, int maxw,
             tui_style st);
void tui_printf(struct tui *t, int x, int y, int maxw, tui_style st,
                const char *fmt, ...);
void tui_box(struct tui *t, int x, int y, int w, int h, tui_style st);
void tui_rule(struct tui *t, int y, char ch, tui_style st);
void tui_flush(struct tui *t);

/** @brief Wait up to @p timeout_ms (-1 = forever) for a key. */
int tui_key(struct tui *t, int timeout_ms);

/* ---- messages and the application loop ------------------------------- */

enum tui_msg_type { TUI_MSG_KEY, TUI_MSG_RESIZE, TUI_MSG_TICK, TUI_MSG_MOUSE };

struct tui_msg {
    enum tui_msg_type type;
    int key;                /**< TUI_MSG_KEY: a byte or one of TUI_KEY_* */
    int w, h;               /**< TUI_MSG_RESIZE: the new size */
    struct tui_mouse mouse; /**< TUI_MSG_MOUSE */
};

struct tui_app {
    void *model;
    /** Handle a message.  Returns true when the screen must be redrawn.
     *  Set @c quit to leave the loop. */
    bool (*update)(struct tui_app *app, const struct tui_msg *m);
    /** Draw the model into the buffer. */
    void (*view)(void *model, struct tui *t);
    int tick_ms; /**< 0 = no tick messages */
    bool quit;
};

/** @brief Run until the app sets @c quit (or the terminal goes away). */
void tui_run(struct tui *t, struct tui_app *app);

/* ---- widgets --------------------------------------------------------- */

/** One scroll window: selected row, first visible row. */
struct tui_viewport {
    int sel;
    int top;
};

void tui_viewport_clamp(struct tui_viewport *v, int rows, int height);
void tui_viewport_move(struct tui_viewport *v, int delta, int rows, int height);
bool tui_viewport_key(struct tui_viewport *v, int key, int rows, int height);

/** A column of a table.  Width counts cells; right aligns numbers. */
struct tui_col {
    const char *title;
    int width;
    bool right;
};

struct tui_table {
    const struct tui_col *cols;
    int ncols;
    struct tui_viewport vp;
};

/** Render one cell into @p buf; the buffer is TUI_CELL_MAX bytes. */
#define TUI_CELL_MAX 128
typedef void (*tui_cell_fn)(void *ctx, int row, int col, char *buf, size_t len);
typedef tui_style (*tui_rowstyle_fn)(void *ctx, int row);

/** Draw the header and the visible rows inside @p r.  @p rows may be 0. */
void tui_table_draw(struct tui *t, tui_rect r, const struct tui_table *tb,
                    int rows, tui_cell_fn cell, tui_rowstyle_fn rowstyle,
                    void *ctx);

/** Apply a key to the viewport; true when it was consumed. */
bool tui_table_key(struct tui_table *tb, int key, int rows, int height);

/** A row of tabs; returns the x just past the last one. */
int tui_tabs_draw(struct tui *t, int x, int y, const char *const *labels, int n,
                  int active);

/** @brief Which tab covers cell @p x, or -1.  Tabs start at column 0. */
int tui_tabs_hit(int x, const char *const *labels, int n);

/** Cycle tabs with left/right/tab/backtab; true when it was consumed. */
bool tui_tabs_key(int key, int *active, int n);

/** A single-line text input. */
struct tui_input {
    char buf[256];
    int len;
    int pos;
    bool focus;
};

void tui_input_draw(struct tui *t, tui_rect r, const struct tui_input *in,
                    tui_style st);
/** Insert, erase, move, home/end; true when the key was consumed. */
bool tui_input_key(struct tui_input *in, int key);

/** A percentage bar, 0-100 or -1 for "no total". */
void tui_progress(struct tui *t, int x, int y, int width, int pct);

/** Fill the bottom row with a help line, left and right halves. */
void tui_statusbar(struct tui *t, const char *left, const char *right);

#endif
