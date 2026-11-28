/**
 * @file tui.h
 * @brief A small terminal application toolkit: cell buffer, diff renderer,
 *        message loop, and a few widgets (viewport, table, tabs, input).
 *
 * Deliberately no ncurses and no terminfo.  Output is plain ANSI (xterm and
 * anything that copies it), input is decoded by hand.  The shape follows the
 * Elm architecture as popularised by Bubble Tea: an application owns a model,
 * receives messages (keys, resize, ticks), and draws from the model.  Only
 * bytes are stored, so a cell is one character wide: no UTF-8.  The frame
 * uses VT100 DEC Special Graphics when the terminal's TERM says it speaks
 * them, and ASCII `+ - |` when it does not (or after tui_alloc alone).  On
 * z/OS the C compiler's EBCDIC literals are converted by the tty, which is
 * why the escape byte is 0x27 there and 0x1b everywhere else.
 */
#ifndef ZTOP_TUI_H
#define ZTOP_TUI_H

#include <stddef.h>
#include <termios.h>

/* The library is C, but a C++ program may include this header. */
#ifdef __cplusplus
extern "C" {
#endif

typedef struct tui_cell {
    unsigned char ch;
    unsigned char fg;
    unsigned char bg;
    unsigned char attr;
} tui_cell;

typedef struct tui_style {
    unsigned char
        fg; /**< xterm-256 index, TUI_COLOR_DEFAULT for the tty's own */
    unsigned char
        bg; /**< xterm-256 index, TUI_COLOR_DEFAULT for the tty's own */
    unsigned char attr; /**< TUI_BOLD / TUI_REVERSE / TUI_DIM / TUI_UNDERLINE */
} tui_style;

typedef struct tui_rect {
    int x, y, w, h;
} tui_rect;

/** Attribute bits, packed into tui_cell.attr. */
enum tui_attr : unsigned char {
    TUI_BOLD = 1,
    TUI_REVERSE = 2,
    TUI_DIM = 4,
    TUI_UNDERLINE = 8,
    /** The byte is a DEC Special Graphics letter, drawn as a line glyph. */
    TUI_ACS = 16,
};

/** The terminal's own foreground/background, as "SGR 39" and "SGR 49". */
enum { TUI_COLOR_DEFAULT = 255 };

/** Background of the selected table row.  A soft surface, because reverse
 * video is the thing that made this look like a curses application. */
enum { TUI_SEL_BG = 236 };

#define TUI_STYLE(f, b, a)                                                     \
    ((tui_style){(unsigned char)(f), (unsigned char)(b), (unsigned char)(a)})
/* Blue, in xterm-256 indices: 25 navy, 18 a shade darker, 75 bright, 111
 * pale, 61 slate, 189 a blue-grey body.  The greens and reds are kept for
 * meaning: free and used, healthy and broken. */
#define TUI_PLAIN TUI_STYLE(189, TUI_COLOR_DEFAULT, 0)
#define TUI_TITLE TUI_STYLE(252, 235, TUI_BOLD)
/* Frames are quiet: a muted grey so the data, not the box, is what the eye
 * lands on.  Accent stays for the focused title and the selection. */
#define TUI_FRAME TUI_STYLE(240, TUI_COLOR_DEFAULT, 0)
#define TUI_LABEL TUI_STYLE(111, TUI_COLOR_DEFAULT, TUI_BOLD)
#define TUI_GOOD TUI_STYLE(84, TUI_COLOR_DEFAULT, 0)
#define TUI_WARN TUI_STYLE(222, TUI_COLOR_DEFAULT, 0)
#define TUI_BAD TUI_STYLE(203, TUI_COLOR_DEFAULT, TUI_BOLD)
#define TUI_TAB_ON TUI_STYLE(16, 75, TUI_BOLD)
#define TUI_TAB_OFF TUI_STYLE(110, TUI_COLOR_DEFAULT, 0)
#define TUI_ACCENT TUI_STYLE(75, TUI_COLOR_DEFAULT, TUI_BOLD)
#define TUI_META TUI_STYLE(61, TUI_COLOR_DEFAULT, 0)
#define TUI_KEYCAP TUI_STYLE(16, 111, TUI_BOLD)
#define TUI_BAR TUI_STYLE(252, 235, 0)

/* Above the byte range, so a key code never collides with a character.
 * Every value is written out: the numbers are not a sequence to maintain. */
enum {
    TUI_KEY_NONE = -1,
    TUI_KEY_ESC = 256,
    TUI_KEY_UP = 257,
    TUI_KEY_DOWN = 258,
    TUI_KEY_LEFT = 259,
    TUI_KEY_RIGHT = 260,
    TUI_KEY_HOME = 261,
    TUI_KEY_END = 262,
    TUI_KEY_PGUP = 263,
    TUI_KEY_PGDN = 264,
    TUI_KEY_DEL = 265,
    TUI_KEY_RESIZE = 266,
    TUI_KEY_TAB = 267,
    TUI_KEY_BACKTAB = 268,
    TUI_KEY_MOUSE = 269,
};

enum { TUI_MAX_W = 500 };
enum { TUI_MAX_H = 200 };

/* ---- layout primitives ----------------------------------------------- */

/** @brief Cut @p r into a top band of @p top_h rows and the rest. */
void tui_split_v(tui_rect r, int top_h, tui_rect *top, tui_rect *bot);
/** @brief Cut @p r into a left band of @p left_w columns and the rest. */
void tui_split_h(tui_rect r, int left_w, tui_rect *left, tui_rect *right);
/** @brief Shrink @p r by @p dx columns and @p dy rows on every side. */
tui_rect tui_inset(tui_rect r, int dx, int dy);
/** @brief True when cell (@p x, @p y) is inside @p r. */
bool tui_rect_hit(tui_rect r, int x, int y);

/** @brief One mouse event; coordinates are 0-based cells. */
typedef struct tui_mouse {
    int x, y;
    int button; /**< 0 left, 1 middle, 2 right, 64 wheel up, 65 wheel down */
    bool release;
} tui_mouse;

typedef struct tui {
    int w, h;
    int fd_in, fd_out;
    tui_cell *cur;
    tui_cell *old;
    bool full;
    bool open;
    bool glyphs; /**< terminal speaks DEC Special Graphics */
    bool acs_on; /**< G0 is currently the DEC graphics set */
    struct termios orig;
    unsigned char in[64];
    int inlen;
    char out[32768];
    int olen;
    tui_mouse mouse;
} tui;

bool tui_alloc(tui *t, int w, int h);
bool tui_open(tui *t);
void tui_close(tui *t);

/** @brief Re-query the size; returns true when it changed and the buffers
 * moved. */
bool tui_resize(tui *t);
void tui_size(tui *t, int *w, int *h);

void tui_clear(tui *t);
void tui_fill(tui *t, int x, int y, int w, int h, char ch, tui_style st);
void tui_put(tui *t, int x, int y, const char *s, int maxw, tui_style st);
void tui_printf(tui *t, int x, int y, int maxw, tui_style st, const char *fmt,
                ...);
void tui_box(tui *t, int x, int y, int w, int h, tui_style st);
/** @brief A box with a title set into its top border. */
void tui_box_frame(tui *t, int x, int y, int w, int h, tui_style st,
                   const char *title, tui_style title_st);
void tui_rule(tui *t, int y, char ch, tui_style st);
void tui_flush(tui *t);

/** @brief Wait up to @p timeout_ms (-1 = forever) for a key. */
int tui_key(tui *t, int timeout_ms);

/* ---- messages and the application loop ------------------------------- */

enum tui_msg_type { TUI_MSG_KEY, TUI_MSG_RESIZE, TUI_MSG_TICK, TUI_MSG_MOUSE };

typedef struct tui_msg {
    enum tui_msg_type type;
    int key;         /**< TUI_MSG_KEY: a byte or one of TUI_KEY_* */
    int w, h;        /**< TUI_MSG_RESIZE: the new size */
    tui_mouse mouse; /**< TUI_MSG_MOUSE */
} tui_msg;

typedef struct tui_app {
    void *model;
    /** Handle a message.  Returns true when the screen must be redrawn.
     *  Set @c quit to leave the loop. */
    bool (*update)(struct tui_app *app, const tui_msg *m);
    /** Draw the model into the buffer. */
    void (*view)(void *model, tui *t);
    int tick_ms; /**< 0 = no tick messages */
    bool quit;
} tui_app;

/** @brief Run until the app sets @c quit (or the terminal goes away). */
void tui_run(tui *t, tui_app *app);

/* ---- widgets --------------------------------------------------------- */

/** One scroll window: selected row, first visible row. */
typedef struct tui_viewport {
    int sel;
    int top;
} tui_viewport;

void tui_viewport_clamp(tui_viewport *v, int rows, int height);
void tui_viewport_move(tui_viewport *v, int delta, int rows, int height);
bool tui_viewport_key(tui_viewport *v, int key, int rows, int height);

/** A column of a table.  Width counts cells; right aligns numbers. */
typedef struct tui_col {
    const char *title;
    int width;
    bool right;
} tui_col;

typedef struct tui_table {
    const tui_col *cols;
    int ncols;
    tui_viewport vp;
} tui_table;

/** Render one cell into @p buf; the buffer is TUI_CELL_MAX bytes. */
enum { TUI_CELL_MAX = 128 };
typedef void (*tui_cell_fn)(void *ctx, int row, int col, char *buf, size_t len);
typedef tui_style (*tui_rowstyle_fn)(void *ctx, int row);

/** Draw the header and the visible rows inside @p r.  @p rows may be 0. */
void tui_table_draw(tui *t, tui_rect r, const tui_table *tb, int rows,
                    tui_cell_fn cell, tui_rowstyle_fn rowstyle, void *ctx);

/** Apply a key to the viewport; true when it was consumed. */
bool tui_table_key(tui_table *tb, int key, int rows, int height);

/** A row of tabs; returns the x just past the last one. */
int tui_tabs_draw(tui *t, int x, int y, const char *const *labels, int n,
                  int active);

/** @brief Which tab covers cell @p x, or -1.  Tabs start at column 0. */
int tui_tabs_hit(int x, const char *const *labels, int n);

/** Cycle tabs with left/right/tab/backtab; true when it was consumed. */
bool tui_tabs_key(int key, int *active, int n);

/** @brief One key binding, for the footer and the help overlay. */
typedef struct tui_binding {
    const char *key;   /**< key cap text, e.g. "TAB", "s S", "/" */
    const char *label; /**< short description */
    const char *group; /**< section title in the help overlay */
    unsigned tabs;     /**< mask of tabs (1u << tab) where it applies */
    bool in_footer;    /**< show in the bottom status bar */
} tui_binding;

/** @brief Draw the footer hints of @p km that apply to @p tab. */
void tui_keymap_footer(tui *t, int x, int y, int maxw, const tui_binding *km,
                       int n, int tab);

/** @brief Draw the grouped help overlay from @p km. */
void tui_keymap_help(tui *t, const tui_binding *km, int n, const char *title);

/** @brief A sparkline of @p n samples, each 0-100, on row @p y. */
enum { TUI_SPARK_LEN = 9 }; /* " .:-=+*#%" */
void tui_sparkline(tui *t, int x, int y, const unsigned char *vals, int n,
                   tui_style st);

/** A single-line text input. */
typedef struct tui_input {
    char buf[256];
    int len;
    int pos;
    bool focus;
} tui_input;

void tui_input_draw(tui *t, tui_rect r, const tui_input *in, tui_style st);
/** Insert, erase, move, home/end; true when the key was consumed. */
bool tui_input_key(tui_input *in, int key);

/** A percentage bar, 0-100 or -1 for "no total". */
void tui_progress(tui *t, int x, int y, int width, int pct);

/** Fill the bottom row with a help line, left and right halves. */
void tui_statusbar(tui *t, const char *left, const char *right);

#ifdef __cplusplus
}
#endif

#endif
