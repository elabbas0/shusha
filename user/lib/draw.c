#include "draw.h"
#include "font8x16.h"

static int imin(int a, int b) { return a < b ? a : b; }
static int imax(int a, int b) { return a > b ? a : b; }

int rect_intersect(rect_t a, rect_t b, rect_t *out) {
    int x0 = imax(a.x, b.x), y0 = imax(a.y, b.y);
    int x1 = imin(a.x + a.w, b.x + b.w), y1 = imin(a.y + a.h, b.y + b.h);
    if (x1 <= x0 || y1 <= y0) return 0;
    out->x = x0;
    out->y = y0;
    out->w = x1 - x0;
    out->h = y1 - y0;
    return 1;
}

rect_t rect_union(rect_t a, rect_t b) {
    rect_t r;
    if (a.w <= 0 || a.h <= 0) return b;
    if (b.w <= 0 || b.h <= 0) return a;
    r.x = imin(a.x, b.x);
    r.y = imin(a.y, b.y);
    r.w = imax(a.x + a.w, b.x + b.w) - r.x;
    r.h = imax(a.y + a.h, b.y + b.h) - r.y;
    return r;
}

int rect_contains(rect_t r, int x, int y) {
    return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h;
}

static int clip(surface_t *s, int *x, int *y, int *w, int *h) {
    rect_t r = { *x, *y, *w, *h }, all = { 0, 0, s->w, s->h }, o;
    if (!rect_intersect(r, all, &o)) return 0;
    *x = o.x;
    *y = o.y;
    *w = o.w;
    *h = o.h;
    return 1;
}

void draw_fill(surface_t *s, int x, int y, int w, int h, uint32_t c) {
    if (!clip(s, &x, &y, &w, &h)) return;
    for (int j = 0; j < h; j++) {
        uint32_t *row = s->px + (y + j) * s->stride + x;
        for (int i = 0; i < w; i++) row[i] = c;
    }
}

void draw_frame(surface_t *s, int x, int y, int w, int h, uint32_t c) {
    draw_fill(s, x, y, w, 1, c);
    draw_fill(s, x, y + h - 1, w, 1, c);
    draw_fill(s, x, y, 1, h, c);
    draw_fill(s, x + w - 1, y, 1, h, c);
}

/* Rounded rectangle: each corner row is inset along a circle of radius r. */
void draw_round_fill(surface_t *s, int x, int y, int w, int h, int r, uint32_t c) {
    if (r * 2 > h) r = h / 2;
    if (r * 2 > w) r = w / 2;
    for (int j = 0; j < h; j++) {
        int d = j < r ? r - j - 1 : (j >= h - r ? j - (h - r) : -1);
        int in = 0;
        if (d >= 0) {
            int t = r * r - d * d, q = 0;
            while ((q + 1) * (q + 1) <= t) q++;
            in = r - q;
        }
        draw_fill(s, x + in, y + j, w - 2 * in, 1, c);
    }
}

static uint32_t mix(uint32_t a, uint32_t b, int t, int n) {
    int ra = (a >> 16) & 255, ga = (a >> 8) & 255, ba = a & 255;
    int rb = (b >> 16) & 255, gb = (b >> 8) & 255, bb = b & 255;
    int r = ra + (rb - ra) * t / n, g = ga + (gb - ga) * t / n, bl = ba + (bb - ba) * t / n;
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)bl;
}

void draw_vgradient(surface_t *s, int x, int y, int w, int h, uint32_t top, uint32_t bottom) {
    for (int j = 0; j < h; j++) draw_fill(s, x, y + j, w, 1, mix(top, bottom, j, h > 1 ? h - 1 : 1));
}

void draw_char(surface_t *s, int x, int y, char ch, uint32_t fg) {
    const unsigned char *g;
    if ((unsigned char)ch < FONT_FIRST || (unsigned char)ch > FONT_LAST) ch = '?';
    if (x >= s->w || y >= s->h || x + GLYPH_W <= 0 || y + GLYPH_H <= 0) return;
    g = font_data[(unsigned char)ch - FONT_FIRST];
    for (int j = 0; j < GLYPH_H; j++) {
        int py = y + j;
        unsigned bits = g[j];
        if (!bits || py < 0 || py >= s->h) continue;
        for (int i = 0; i < GLYPH_W; i++) {
            int px = x + i;
            if ((bits & (0x80u >> i)) && px >= 0 && px < s->w) s->px[py * s->stride + px] = fg;
        }
    }
}

int text_width(const char *text) {
    int n = 0;
    while (text[n]) n++;
    return n * GLYPH_W;
}

int draw_text(surface_t *s, int x, int y, const char *text, uint32_t fg) {
    int x0 = x;
    for (; *text; text++, x += GLYPH_W) draw_char(s, x, y, *text, fg);
    return x - x0;
}

void draw_text_clip(surface_t *s, int x, int y, const char *text, uint32_t fg, int max_px) {
    int max_chars = max_px / GLYPH_W, n = 0;
    while (text[n]) n++;
    if (n <= max_chars) {
        draw_text(s, x, y, text, fg);
        return;
    }
    for (int i = 0; i < max_chars - 3 && i < n; i++) draw_char(s, x + i * GLYPH_W, y, text[i], fg);
    for (int i = 0; i < 3 && max_chars >= 3; i++) draw_char(s, x + (max_chars - 3 + i) * GLYPH_W, y, '.', fg);
}

void draw_blit(surface_t *dst, int x, int y, const uint32_t *src, int src_stride, int w, int h) {
    int sx = 0, sy = 0;
    if (x < 0) { sx = -x; w += x; x = 0; }
    if (y < 0) { sy = -y; h += y; y = 0; }
    if (x + w > dst->w) w = dst->w - x;
    if (y + h > dst->h) h = dst->h - y;
    if (w <= 0 || h <= 0) return;
    for (int j = 0; j < h; j++) {
        const uint32_t *s = src + (sy + j) * src_stride + sx;
        uint32_t *d = dst->px + (y + j) * dst->stride + x;
        for (int i = 0; i < w; i++) d[i] = s[i];
    }
}

/* ---- widgets ---- */

void ui_button(surface_t *s, int x, int y, int w, int h, const char *label, int active, int hover) {
    uint32_t fill = active ? COL_ACCENT : (hover ? COL_PANEL_HI : COL_PANEL);
    uint32_t fg = active ? 0x0b1117u : COL_TEXT;
    int tw = text_width(label);
    draw_round_fill(s, x, y, w, h, 4, active ? COL_ACCENT : COL_EDGE);
    draw_round_fill(s, x + 1, y + 1, w - 2, h - 2, 3, fill);
    draw_text_clip(s, x + (w - tw > 8 ? (w - tw) / 2 : 4), y + (h - GLYPH_H) / 2, label, fg, w - 8);
}

#define MENU_ITEM_H 24
#define MENU_PAD 4

int menu_width(const menu_t *m) {
    int w = 0;
    for (int i = 0; i < m->count; i++) {
        int t = text_width(m->items[i]);
        if (t > w) w = t;
    }
    return w + 32;
}

int menu_height(const menu_t *m) {
    return m->count * MENU_ITEM_H + 2 * MENU_PAD;
}

void menu_draw(surface_t *s, int x, int y, const menu_t *m, int hover) {
    int w = menu_width(m), h = menu_height(m);
    draw_fill(s, x + 3, y + 3, w, h, 0x05080c);
    draw_round_fill(s, x, y, w, h, 4, COL_EDGE);
    draw_round_fill(s, x + 1, y + 1, w - 2, h - 2, 3, COL_PANEL);
    for (int i = 0; i < m->count; i++) {
        int iy = y + MENU_PAD + i * MENU_ITEM_H;
        if (i == hover) draw_fill(s, x + 3, iy, w - 6, MENU_ITEM_H, COL_SELECT);
        draw_text(s, x + 14, iy + (MENU_ITEM_H - GLYPH_H) / 2, m->items[i], COL_TEXT);
    }
}

int menu_hit(const menu_t *m, int x, int y, int mx, int my) {
    int w = menu_width(m);
    if (mx < x || mx >= x + w || my < y + MENU_PAD) return -1;
    int i = (my - y - MENU_PAD) / MENU_ITEM_H;
    return i < m->count ? i : -1;
}
