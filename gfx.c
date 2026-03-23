#include "gfx.h"
#include "vm.h"
#include <string.h>
#include "raylib.h"

/* Global graphics state */
GfxState gfx = { 255, 255, 255, 255 };   /* default: white */

/* -------------------------------------------------------------------------
 * Text buffer
 * ------------------------------------------------------------------------- */
static TextBuffer tbuf;

/* Return pointer to line at logical index i (0 = oldest). */
static char *tbuf_line(int i) {
    return tbuf.lines[(tbuf.head + i) % TEXT_ROWS];
}

/* Append a new empty line; returns pointer to it. */
static char *tbuf_newline(void) {
    if (tbuf.count < TEXT_ROWS) {
        /* Not yet full: use next slot after head+count */
        char *line = tbuf.lines[(tbuf.head + tbuf.count) % TEXT_ROWS];
        line[0] = '\0';
        tbuf.count++;
        return line;
    } else {
        /* Full: overwrite oldest (advance head) */
        char *line = tbuf.lines[tbuf.head];
        tbuf.head = (tbuf.head + 1) % TEXT_ROWS;
        line[0] = '\0';
        return line;
    }
}

/* Ensure at least one line exists. */
static void tbuf_ensure_line(void) {
    if (tbuf.count == 0)
        tbuf_newline();
}

void gfx_print(const char *s) {
    tbuf_ensure_line();
    for (; *s; s++) {
        if (*s == '\n') {
            tbuf_newline();
            continue;
        }
        /* Append char to current (last) logical line */
        char *line = tbuf_line(tbuf.count - 1);
        int len = (int)strlen(line);
        if (len < TEXT_COLS) {
            line[len]     = *s;
            line[len + 1] = '\0';
        } else {
            /* Line full: open new line and place char there */
            tbuf_newline();
            line = tbuf_line(tbuf.count - 1);
            line[0] = *s;
            line[1] = '\0';
        }
    }
}

/* -------------------------------------------------------------------------
 * gfx_run — Raylib window loop (panels wired in Tasks 9, 10)
 * ------------------------------------------------------------------------- */
void gfx_run(struct VM *vm) {
    (void)vm;
    InitWindow(WINDOW_W, WINDOW_H, "vm32g forth");
    SetTargetFPS(60);

    while (!WindowShouldClose()) {
        BeginDrawing();
            ClearBackground(BLACK);
            /* Divider line between panels */
            DrawLine(PANEL_SPLIT, 0, PANEL_SPLIT, WINDOW_H, DARKGRAY);
        EndDrawing();
    }
    CloseWindow();
}
