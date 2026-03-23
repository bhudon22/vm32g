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
 * Drawing stubs — replaced with real Raylib calls in Task 10
 * ------------------------------------------------------------------------- */
void gfx_draw_pixel(int x, int y)                         { (void)x; (void)y; }
void gfx_draw_line(int x1, int y1, int x2, int y2)        { (void)x1; (void)y1; (void)x2; (void)y2; }
void gfx_draw_rect(int x, int y, int w, int h)            { (void)x; (void)y; (void)w; (void)h; }
void gfx_draw_circle(int x, int y, int r)                 { (void)x; (void)y; (void)r; }
void gfx_clear_canvas(void)                               {}
int  gfx_key_pressed(void)                                { return 0; }
int  gfx_last_key(void)                                   { return 0; }
int  gfx_key_down(int keycode)                            { (void)keycode; return 0; }

/* -------------------------------------------------------------------------
 * gfx_run — Raylib window loop (panels wired in Tasks 9, 10)
 * ------------------------------------------------------------------------- */

/* REPL input state */
static char  input_buf[256];
static int   input_len = 0;

/* Render left panel: text buffer + input line */
static void draw_left_panel(void) {
    /* Panel background */
    DrawRectangle(0, 0, PANEL_SPLIT, WINDOW_H, (Color){20, 20, 30, 255});

    int x = 4;
    int y = 4;
    int font = TEXT_FONT_SIZE;

    /* Show last TEXT_VISIBLE lines from the buffer */
    int start = tbuf.count > TEXT_VISIBLE ? tbuf.count - TEXT_VISIBLE : 0;
    for (int i = start; i < tbuf.count; i++) {
        DrawText(tbuf_line(i), x, y, font, (Color){200, 200, 200, 255});
        y += font + 2;
    }

    /* Input line with cursor */
    int input_y = WINDOW_H - font - 6;
    DrawText("> ", x, input_y, font, (Color){100, 220, 100, 255});
    DrawText(input_buf, x + 16, input_y, font, (Color){255, 255, 255, 255});
    /* Blinking cursor */
    if ((GetTime() * 2.0 - (int)(GetTime() * 2.0)) < 0.5) {
        int cursor_x = x + 16 + MeasureText(input_buf, font);
        DrawText("_", cursor_x, input_y, font, (Color){255, 255, 100, 255});
    }
}

/* Handle keyboard input for REPL */
static void handle_repl_input(VM *vm) {
    /* Printable characters */
    int c;
    while ((c = GetCharPressed()) > 0) {
        if (c >= 32 && c <= 126 && input_len < 255) {
            input_buf[input_len++] = (char)c;
            input_buf[input_len]   = '\0';
        }
    }

    /* Backspace */
    if (IsKeyPressed(KEY_BACKSPACE) || IsKeyPressedRepeat(KEY_BACKSPACE)) {
        if (input_len > 0) {
            input_buf[--input_len] = '\0';
        }
    }

    /* Enter — evaluate */
    if (IsKeyPressed(KEY_ENTER)) {
        /* Echo the input line */
        gfx_print("> ");
        gfx_print(input_buf);
        gfx_print("\n");

        if (input_len > 0) {
            int result = vm_eval(vm, input_buf);
            if (result == 0)
                gfx_print("ok\n");
            /* vm_eval already prints error details via gfx_print in vm.c */
        }

        input_buf[0] = '\0';
        input_len = 0;
    }
}

void gfx_run(VM *vm) {
    InitWindow(WINDOW_W, WINDOW_H, "vm32g forth");
    SetTargetFPS(60);

    while (!WindowShouldClose()) {
        /* --- Update phase --- */
        handle_repl_input(vm);

        /* Call Forth 'update' word if defined */
        uint32_t xt = dict_find(vm, "update", 6);
        if (xt) { vm->rsp = 0; vm->ip = dict_body(vm, xt); vm_run(vm); }

        /* --- Draw phase --- */
        BeginDrawing();
            ClearBackground(BLACK);

            /* Call Forth 'draw' word if defined */
            xt = dict_find(vm, "draw", 4);
            if (xt) { vm->rsp = 0; vm->ip = dict_body(vm, xt); vm_run(vm); }

            /* Render left panel on top */
            draw_left_panel();

            /* Panel divider */
            DrawLine(PANEL_SPLIT, 0, PANEL_SPLIT, WINDOW_H, DARKGRAY);
        EndDrawing();
    }

    CloseWindow();
}
