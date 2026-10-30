#ifndef SHUSHA_DRAW_H
#define SHUSHA_DRAW_H

/* Software drawing for the WM and apps: 32bpp 0x00RRGGBB surfaces,
 * everything clipped to the surface.  Shapes and text are anti-aliased
 * and alpha-blended; text is UTF-8 in three faces (sans, bold, mono). */

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
#define COL_BG        0x10151du
#define COL_PANEL     0x171e28u
#define COL_PANEL_HI  0x222b37u
#define COL_EDGE      0x2c3646u
#define COL_TEXT      0xe6edf5u
#define COL_MUTED     0x8b98aau
#define COL_ACCENT    0x3fb6a8u
#define COL_ACCENT_HI 0x5fd4c5u
#define COL_DANGER    0xe5645au
#define COL_WARN      0xe6b450u
#define COL_SELECT    0x24465au

/* Monospace cell (terminal grid, editor) and the shared line height. */
#define GLYPH_W 8
#define GLYPH_H 16

enum { FONT_SANS, FONT_BOLD, FONT_MONO };

/* Corner selection for draw_rrect. */
#define RR_TL 1
#define RR_TR 2
#define RR_BL 4
#define RR_BR 8
#define RR_ALL 15

int  rect_intersect(rect_t a, rect_t b, rect_t *out);
rect_t rect_union(rect_t a, rect_t b);
int  rect_contains(rect_t r, int x, int y);

/* Blend src over dst with alpha 0..255. */
static inline uint32_t blend(uint32_t dst, uint32_t src, uint32_t a) {
    uint32_t a1 = a + (a >> 7), a0 = 256 - a1;
    uint32_t rb = ((src & 0xff00ffu) * a1 + (dst & 0xff00ffu) * a0) >> 8;
    uint32_t g = ((src & 0x00ff00u) * a1 + (dst & 0x00ff00u) * a0) >> 8;
    return (rb & 0xff00ffu) | (g & 0x00ff00u);
}

void draw_fill(surface_t *s, int x, int y, int w, int h, uint32_t c);
void draw_fill_a(surface_t *s, int x, int y, int w, int h, uint32_t c, uint32_t a);
void draw_frame(surface_t *s, int x, int y, int w, int h, uint32_t c);
/* Anti-aliased rounded rectangle; `corners` picks which corners round. */
void draw_rrect(surface_t *s, int x, int y, int w, int h, int r, int corners, uint32_t c, uint32_t a);
void draw_rrect_outline(surface_t *s, int x, int y, int w, int h, int r, uint32_t c, uint32_t a);
void draw_round_fill(surface_t *s, int x, int y, int w, int h, int r, uint32_t c);
void draw_circle(surface_t *s, int cx, int cy, int r, uint32_t c, uint32_t a);
/* Soft shadow around a rounded rectangle, fading out over `size` px. */
void draw_shadow(surface_t *s, int x, int y, int w, int h, int r, int size, uint32_t c, uint32_t a);
void draw_vgradient(surface_t *s, int x, int y, int w, int h, uint32_t top, uint32_t bottom);
/* Premultiplied 0xAARRGGBB image over the surface. */
void draw_image(surface_t *s, int x, int y, const uint32_t *px, int w, int h, int stride);
void draw_blit(surface_t *dst, int x, int y, const uint32_t *src, int src_stride, int w, int h);

/* Text: y is the top of a GLYPH_H line box.  Returns width in pixels. */
int  draw_text_f(surface_t *s, int x, int y, const char *utf8, uint32_t fg, int font);
int  text_width_f(const char *utf8, int font);
/* Width of the first `bytes` bytes of `utf8`. */
int  text_width_n(const char *utf8, int bytes, int font);
/* Draw at most max_px wide, ending in an ellipsis when cut. */
void draw_text_clip_f(surface_t *s, int x, int y, const char *utf8, uint32_t fg, int max_px, int font);

/* Sans-serif shorthands and the monospace cell. */
int  draw_text(surface_t *s, int x, int y, const char *text, uint32_t fg);
int  text_width(const char *text);
void draw_text_clip(surface_t *s, int x, int y, const char *text, uint32_t fg, int max_px);
void draw_char(surface_t *s, int x, int y, char ch, uint32_t fg);

enum { ICON_FOLDER, ICON_FILE, ICON_APP, ICON_TERMINAL, ICON_POWER };
#define ICON_SIZE 48
void draw_icon(surface_t *s, int x, int y, int icon);
void draw_cursor(surface_t *s, int x, int y);
int  cursor_width(void);
int  cursor_height(void);

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
/* Area a menu at (x, y) touches, including its shadow. */
rect_t menu_bounds(const menu_t *m, int x, int y);

#endif
