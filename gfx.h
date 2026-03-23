#ifndef GFX_H
#define GFX_H

#include <stdint.h>

/* -------------------------------------------------------------------------
 * Forward declaration — avoids including vm.h here
 * ------------------------------------------------------------------------- */
struct VM;

/* -------------------------------------------------------------------------
 * Layout constants
 * ------------------------------------------------------------------------- */
#define WINDOW_W       1280
#define WINDOW_H       612
#define PANEL_SPLIT    512      /* left panel width (40%) */
#define CANVAS_W       768      /* right panel width (60%) */
#define CANVAS_H       612
#define CANVAS_CX      384      /* canvas centre X in texture space */
#define CANVAS_CY      306      /* canvas centre Y in texture space */

/* -------------------------------------------------------------------------
 * Text panel
 * ------------------------------------------------------------------------- */
#define TEXT_COLS      64       /* max chars per line */
#define TEXT_ROWS      100      /* scrollback lines */
#define TEXT_FONT_SIZE 14
#define TEXT_VISIBLE   ((WINDOW_H - 30) / TEXT_FONT_SIZE)  /* ~49 visible rows */

typedef struct {
    char   lines[TEXT_ROWS][TEXT_COLS + 1];
    int    count;       /* total lines stored (wraps at TEXT_ROWS) */
    int    head;        /* index of oldest line (ring buffer) */
} TextBuffer;

/* -------------------------------------------------------------------------
 * Graphics state (shared with vm.c opcode handlers)
 * ------------------------------------------------------------------------- */
typedef struct {
    uint8_t  color_r, color_g, color_b, color_a;   /* current draw colour */
} GfxState;

extern GfxState gfx;

/* -------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------- */

/* Append text to the left-panel buffer. Handles \n. */
void gfx_print(const char *s);

/* Open window, run frame loop until window is closed. */
void gfx_run(struct VM *vm);

/* Called from vm.c opcode handlers */
void gfx_draw_pixel(int x, int y);
void gfx_draw_line(int x1, int y1, int x2, int y2);
void gfx_draw_rect(int x, int y, int w, int h);
void gfx_draw_circle(int x, int y, int r);
void gfx_clear_canvas(void);
int  gfx_key_pressed(void);
int  gfx_last_key(void);
int  gfx_key_down(int keycode);

#endif /* GFX_H */
