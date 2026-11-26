/* Buffer-level tests for libtui: no terminal, no clock, no signals.
 * `make test` builds and runs this next to the library. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tui.h"

static int fails;

#define CHECK(c)                                                               \
    do {                                                                       \
        if (!(c)) {                                                            \
            (void)printf("FAIL line %d: %s\n", __LINE__, #c);                  \
            fails++;                                                           \
        }                                                                      \
    } while (0)

static unsigned char cell(const struct tui *t, int x, int y) {
    return t->cur[(size_t)y * (size_t)t->w + (size_t)x].ch;
}

static void test_buffer(void) {
    struct tui t;

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
    struct tui_viewport v = {0, 0};

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

    struct tui t;

    memset(&t, 0, sizeof t);
    CHECK(tui_alloc(&t, 30, 2));
    static const char *const labels[3] = {"one", "two", "three"};
    int x = tui_tabs_draw(&t, 0, 0, labels, 3, 1);
    CHECK(x == 5 + 1 + 5 + 1 + 7);
    CHECK(cell(&t, 0, 0) == ' ');
    CHECK(cell(&t, 1, 0) == 'o');
    CHECK(cell(&t, 6, 0) == ' ');
    CHECK(cell(&t, 7, 0) == 't');
    free(t.cur);
    free(t.old);
}

static void test_input(void) {
    struct tui_input in;

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
    static const struct tui_col cols[2] = {
        {"A", 5, false},
        {"B", 4, true},
    };
    struct tui_table tb = {cols, 2, {0, 0}};
    struct tui t;

    memset(&t, 0, sizeof t);
    CHECK(tui_alloc(&t, 20, 5));
    tui_table_draw(&t, (tui_rect){0, 0, 20, 5}, &tb, 3, called, NULL, NULL);
    CHECK(cell(&t, 0, 0) == 'A');
    CHECK(cell(&t, 6, 0) == ' ');
    CHECK(cell(&t, 6, 1) == 'r'); /* first data row, first column */
    CHECK(cell(&t, 8, 1) == 'c');
    tb.vp.sel = 2;
    tui_table_draw(&t, (tui_rect){0, 0, 20, 5}, &tb, 3, called, NULL, NULL);
    CHECK(cell(&t, 6, 3) == 'r'); /* third row is last visible */
    free(t.cur);
    free(t.old);
}

int main(void) {
    test_buffer();
    test_viewport();
    test_tabs();
    test_input();
    test_table();
    (void)printf("test_tui: %s\n", fails == 0 ? "ok" : "FAILED");
    return fails != 0;
}
