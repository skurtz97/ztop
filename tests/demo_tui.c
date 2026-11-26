/* A tiny exercise of src/tui.c: boxes, colors, keys, resize.  Not installed;
 * run it on any terminal with `make demo && ./tests/demo_tui`. */
#include <stdio.h>
#include <string.h>

#include "../src/tui.h"

int main(void)
{
    struct tui t;
    int x = 4;

    if (!tui_open(&t)) {
        (void)fprintf(stderr, "demo_tui: needs a terminal\n");
        return 77;
    }
    for (;;) {
        int w, h;

        tui_size(&t, &w, &h);
        tui_clear(&t);
        tui_fill(&t, 0, 0, w, 1, ' ', TUI_TITLE);
        tui_printf(&t, 1, 0, w - 2, TUI_TITLE, " tui demo  %dx%d", w, h);
        tui_box(&t, 1, 2, w - 2, h - 4, TUI_LABEL);
        if (w > 50 && h > 12) {
            tui_printf(&t, 3, 3, w - 6, TUI_PLAIN,
                       "arrows move the bar, r resizes, q quits");
            tui_printf(&t, 3, 5, w - 6, TUI_GOOD, "position: %d", x);
            tui_fill(&t, 3, 7, (w - 8) * x / 20, 1, '#', TUI_GOOD);
            tui_box(&t, w / 2 - 10, h / 2 - 3, 22, 7, TUI_WARN);
            tui_printf(&t, w / 2 - 8, h / 2 - 1, 18, TUI_WARN, "a box");
        } else {
            tui_printf(&t, 1, 3, w - 2, TUI_BAD, "too small");
        }
        tui_flush(&t);

        int k = tui_key(&t, 200);
        if (k == 'q' || k == 0x03)
            break;
        if (k == TUI_KEY_RESIZE || k == 'r')
            (void)tui_resize(&t);
        else if (k == TUI_KEY_RIGHT && x < 20)
            x++;
        else if (k == TUI_KEY_LEFT && x > 0)
            x--;
    }
    tui_close(&t);
    return 0;
}
