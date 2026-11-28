/* Buffer-level tests for libtui: no terminal, no signals.  The tick test
 * uses a pipe and a real clock, because that is where the bug was.
 * `make test` builds and runs this next to the library. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <unistd.h>

#include "tui.h"

static int fails;

#define CHECK(c)                                                               \
    do {                                                                       \
        if (!(c)) {                                                            \
            (void)printf("FAIL line %d: %s\n", __LINE__, #c);                  \
            fails++;                                                           \
        }                                                                      \
    } while (0)

static unsigned char cell(const tui *t, int x, int y) {
    return t->cur[(size_t)y * (size_t)t->w + (size_t)x].ch;
}

static void test_buffer(void) {
    tui t;

    memset(&t, 0, sizeof t);
    CHECK(tui_alloc(&t, 20, 6));
    tui_fill(&t, 0, 0, 20, 6, ' ', TUI_PLAIN);
    CHECK(cell(&t, 0, 0) == ' ');

    tui_put(&t, 18, 0, "abc", 5, TUI_PLAIN);
    CHECK(cell(&t, 18, 0) == 'a');
    CHECK(cell(&t, 19, 0) == 'b');

    tui_printf(&t, 2, 1, 3, TUI_PLAIN, "%d", 12345);
    CHECK(cell(&t, 2, 1) == '1');
    CHECK(cell(&t, 4, 1) == '3');
    CHECK(cell(&t, 5, 1) == ' ');

    tui_fill(&t, -3, -3, 5, 5, 'x', TUI_PLAIN);
    CHECK(cell(&t, 0, 0) == 'x');
    CHECK(cell(&t, 1, 0) == 'x');
    CHECK(cell(&t, 2, 0) == ' ');

    tui_rule(&t, 5, '-', TUI_PLAIN);
    CHECK(cell(&t, 0, 5) == '-');
    CHECK(cell(&t, 19, 5) == '-');

    tui_box(&t, 0, 2, 10, 3, TUI_PLAIN);
    CHECK(cell(&t, 0, 2) == '+');
    CHECK(cell(&t, 9, 2) == '+');

    free(t.cur);
    free(t.old);
}

static void test_viewport(void) {
    tui_viewport v = {0, 0};

    tui_viewport_clamp(&v, 10, 3);
    CHECK(v.sel == 0 && v.top == 0);
    tui_viewport_move(&v, 5, 10, 3);
    CHECK(v.sel == 5 && v.top == 3);
    tui_viewport_move(&v, -100, 10, 3);
    CHECK(v.sel == 0 && v.top == 0);
    CHECK(tui_viewport_key(&v, TUI_KEY_END, 10, 3));
    CHECK(v.sel == 9 && v.top == 7);
    CHECK(tui_viewport_key(&v, TUI_KEY_PGUP, 10, 3));
    CHECK(v.sel == 6 && v.top == 6);
    CHECK(!tui_viewport_key(&v, 'z', 10, 3));
    /* empty list must not go negative */
    v.sel = 3;
    tui_viewport_clamp(&v, 0, 4);
    CHECK(v.sel == 0 && v.top == 0);
}

static void test_tabs(void) {
    int act = 0;

    CHECK(tui_tabs_key(TUI_KEY_TAB, &act, 3));
    CHECK(act == 1);
    CHECK(tui_tabs_key(TUI_KEY_BACKTAB, &act, 3));
    CHECK(act == 0);
    CHECK(tui_tabs_key(TUI_KEY_LEFT, &act, 3));
    CHECK(act == 2);
    CHECK(!tui_tabs_key('x', &act, 3));

    tui t;

    memset(&t, 0, sizeof t);
    CHECK(tui_alloc(&t, 30, 2));
    static const char *const labels[3] = {"one", "two", "three"};
    int x = tui_tabs_draw(&t, 0, 0, labels, 3, 1);
    CHECK(x == 5 + 2 + 5 + 2 + 7);
    CHECK(cell(&t, 0, 0) == ' ');
    CHECK(cell(&t, 1, 0) == 'o');
    CHECK(cell(&t, 6, 0) == ' ');
    CHECK(cell(&t, 7, 0) == ' ');
    CHECK(cell(&t, 8, 0) == 't');
    CHECK(tui_tabs_hit(8, labels, 3) == 1);
    CHECK(tui_tabs_hit(6, labels, 3) == -1);
    free(t.cur);
    free(t.old);
}

static void test_input(void) {
    tui_input in;

    memset(&in, 0, sizeof in);
    in.focus = true;
    CHECK(tui_input_key(&in, 'a'));
    CHECK(tui_input_key(&in, 'b'));
    CHECK(strcmp(in.buf, "ab") == 0 && in.len == 2 && in.pos == 2);
    CHECK(tui_input_key(&in, TUI_KEY_LEFT));
    CHECK(tui_input_key(&in, 'X'));
    CHECK(strcmp(in.buf, "aXb") == 0 && in.pos == 2);
    CHECK(tui_input_key(&in, TUI_KEY_DEL));
    CHECK(strcmp(in.buf, "aX") == 0 && in.pos == 2);
    CHECK(tui_input_key(&in, TUI_KEY_BACKTAB) == false);
    CHECK(tui_input_key(&in, 8));
    CHECK(strcmp(in.buf, "a") == 0 && in.pos == 1);
    CHECK(tui_input_key(&in, TUI_KEY_END));
    CHECK(in.pos == 1);
    in.focus = false;
    CHECK(!tui_input_key(&in, 'z'));
}

/* Rows come from a callback; check the table maps row/col and clips. */
static void called(void *ctx, int row, int col, char *buf, size_t len) {
    (void)ctx;
    (void)snprintf(buf, len, "r%dc%d", row, col);
}

static void test_table(void) {
    static const tui_col cols[2] = {
        {"A", 5, false},
        {"B", 4, true},
    };
    tui_table tb = {cols, 2, {0, 0}};
    tui t;

    memset(&t, 0, sizeof t);
    CHECK(tui_alloc(&t, 20, 5));
    tui_table_draw(&t, (tui_rect){0, 0, 20, 5}, &tb, 3, called, nullptr,
                   nullptr);
    CHECK(cell(&t, 0, 0) == 'A');
    CHECK(cell(&t, 6, 0) == ' ');
    CHECK(cell(&t, 6, 1) == 'r'); /* first data row, first column */
    CHECK(cell(&t, 8, 1) == 'c');
    tb.vp.sel = 2;
    tui_table_draw(&t, (tui_rect){0, 0, 20, 5}, &tb, 3, called, nullptr,
                   nullptr);
    CHECK(cell(&t, 6, 3) == 'r'); /* third row is last visible */
    free(t.cur);
    free(t.old);
}

/* Layout, sparkline and keymap: geometry is computed once and shared. */
static void test_widgets(void) {
    tui t;
    tui_rect a, b, r = {0, 0, 80, 24};
    static const unsigned char samples[] = {0, 50, 100};
    static const tui_binding km[] = {
        {"q", "quit", "screen", 0xFu, true},
    };

    tui_split_v(r, 3, &a, &b);
    CHECK(a.y == 0 && a.h == 3 && b.y == 3 && b.h == 21);
    tui_split_h(b, 50, &a, &b);
    CHECK(a.x == 0 && a.w == 50 && b.x == 50 && b.w == 30);
    a = tui_inset(r, 2, 1);
    CHECK(a.x == 2 && a.y == 1 && a.w == 76 && a.h == 22);
    CHECK(tui_rect_hit(a, 2, 1));
    CHECK(!tui_rect_hit(a, 1, 1));
    CHECK(!tui_rect_hit(a, 78, 1));

    memset(&t, 0, sizeof t);
    CHECK(tui_alloc(&t, 40, 6));
    tui_clear(&t);
    tui_sparkline(&t, 0, 0, samples, 3, TUI_ACCENT);
    CHECK(cell(&t, 0, 0) == ' ');
    CHECK(cell(&t, 1, 0) == '=');
    CHECK(cell(&t, 2, 0) == '%');
    tui_keymap_footer(&t, 0, 1, 40, km, 1, 0);
    CHECK(cell(&t, 1, 1) == 'q');
    free(t.cur);
    free(t.old);
}

typedef struct tick_state {
    int ticks;
    int keys;
} tick_state;

static void nothing(void *model, tui *t) {
    (void)model, (void)t;
}

static bool tick_update(tui_app *app, const tui_msg *m) {
    tick_state *s = app->model;

    if (m->type == TUI_MSG_TICK) {
        s->ticks++;
        app->quit = s->ticks >= 2;
        return false;
    }
    if (m->type == TUI_MSG_KEY) {
        s->keys++;
        if (s->keys >= 100) /* the bug: keys all the way and not a tick */
            app->quit = true;
    }
    return false;
}

/* The interval is a deadline, not a wait restarted by every key.  A child
 * holds a key down at 5 ms intervals; with the old code the 50 ms select
 * never expired and no tick arrived. */
static void test_tick(void) {
    tui t;
    tui_app app;
    tick_state s;
    int p[2];
    pid_t pid;

    memset(&t, 0, sizeof t);
    memset(&app, 0, sizeof app);
    memset(&s, 0, sizeof s);
    CHECK(pipe(p) == 0);
    CHECK(tui_alloc(&t, 20, 6));
    t.fd_in = p[0];
    t.fd_out = -1; /* nothing redraws, so nothing is flushed */
    app.model = &s;
    app.update = tick_update;
    app.view = nothing;
    app.tick_ms = 50;

    pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) {
        int idle[2];

        (void)close(p[0]);
        if (pipe(idle) != 0)
            _exit(1);
        for (int i = 0; i < 200; i++) {
            /* select is the sleep this build has: nanosleep is not
             * declared on z/OS, and nfds 0 with no set returns at once. */
            fd_set f;
            struct timeval gap = {0, 5000};

            if (write(p[1], "j", 1) != 1)
                break;
            FD_ZERO(&f);
            FD_SET(idle[0], &f);
            (void)select(idle[0] + 1, &f, nullptr, nullptr, &gap);
        }
        _exit(0);
    }
    (void)close(p[1]);
    tui_run(&t, &app);
    CHECK(s.ticks == 2);
    CHECK(s.keys < 100);

    (void)close(p[0]); /* the writer stops on the next write */
    (void)waitpid(pid, nullptr, 0);
    free(t.cur);
    free(t.old);
}

int main(void) {
    test_buffer();
    test_viewport();
    test_tabs();
    test_input();
    test_table();
    test_widgets();
    test_tick();
    (void)printf("test_tui: %s\n", fails == 0 ? "ok" : "FAILED");
    return fails != 0;
}
