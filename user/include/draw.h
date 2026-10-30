#ifndef SHUSHA_DRAW_H
#define SHUSHA_DRAW_H

/* Software drawing for the WM and apps: 32bpp 0x00RRGGBB surfaces,
 * everything clipped to the surface, 8x16 bitmap font. */

#include <stdint.h>

typedef struct {
    uint32_t *px;
    int w, h;
    int stride; /* pixels per row */
} surface_t;

typedef struct {
    int x, y, w, h;
} rect_t;

/* Theme */
#define COL_BG        0x0f141cu
#define COL_PANEL     0x161d29u
#define COL_PANEL_HI  0x1f2937u
#define COL_EDGE      0x2b3647u
#define COL_TEXT      0xe6edf5u
#define COL_MUTED     0x8b98aau
#define COL_ACCENT    0x3fb6a8u
#define COL_ACCENT_HI 0x5fd4c5u
#define COL_DANGER    0xe5645au
#define COL_WARN      0xe6b450u
#define COL_SELECT    0x24465au

#define GLYPH_W 8
#define GLYPH_H 16

int  rect_intersect(rect_t a, rect_t b, rect_t *out);
rect_t rect_union(rect_t a, rect_t b);
int  rect_contains(rect_t r, int x, int y);

void draw_fill(surface_t *s, int x, int y, int w, int h, uint32_t c);
void draw_frame(surface_t *s, int x, int y, int w, int h, uint32_t c);
void draw_round_fill(surface_t *s, int x, int y, int w, int h, int r, uint32_t c);
void draw_vgradient(surface_t *s, int x, int y, int w, int h, uint32_t top, uint32_t bottom);
void draw_char(surface_t *s, int x, int y, char ch, uint32_t fg);
/* Draw text (transparent background); returns the width in pixels. */
int  draw_text(surface_t *s, int x, int y, const char *text, uint32_t fg);
/* Draw at most max_px wide, ending with "..." when cut. */
void draw_text_clip(surface_t *s, int x, int y, const char *text, uint32_t fg, int max_px);
int  text_width(const char *text);
/* Copy a w*h block from src (with its own stride) to dst at (x, y). */
void draw_blit(surface_t *dst, int x, int y, const uint32_t *src, int src_stride, int w, int h);

/* ---- widgets ---- */
void ui_button(surface_t *s, int x, int y, int w, int h, const char *label, int active, int hover);

#define MENU_MAX 10
typedef struct {
    const char *items[MENU_MAX];
    int count;
} menu_t;

int  menu_width(const menu_t *m);
int  menu_height(const menu_t *m);
void menu_draw(surface_t *s, int x, int y, const menu_t *m, int hover);
/* Index of the item under (mx, my), or -1. */
int  menu_hit(const menu_t *m, int x, int y, int mx, int my);

#endif
