#include "draw.h"
#include "ui_assets.h"

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

static uint32_t isqrt(uint32_t v) {
    uint32_t r = 0, bit = 1u << 30;
    while (bit > v) bit >>= 2;
    while (bit) {
        if (v >= r + bit) {
            v -= r + bit;
            r = (r >> 1) + bit;
        } else {
            r >>= 1;
        }
        bit >>= 2;
    }
    return r;
}

static inline void put(surface_t *s, int x, int y, uint32_t c, uint32_t a) {
    uint32_t *p = s->px + y * s->stride + x;
    *p = a >= 255 ? c : blend(*p, c, a);
}

void draw_fill(surface_t *s, int x, int y, int w, int h, uint32_t c) {
    if (!clip(s, &x, &y, &w, &h)) return;
    for (int j = 0; j < h; j++) {
        uint32_t *row = s->px + (y + j) * s->stride + x;
        for (int i = 0; i < w; i++) row[i] = c;
    }
}

void draw_fill_a(surface_t *s, int x, int y, int w, int h, uint32_t c, uint32_t a) {
    if (a >= 255) {
        draw_fill(s, x, y, w, h, c);
        return;
    }
    if (!a || !clip(s, &x, &y, &w, &h)) return;
    for (int j = 0; j < h; j++) {
        uint32_t *row = s->px + (y + j) * s->stride + x;
        for (int i = 0; i < w; i++) row[i] = blend(row[i], c, a);
    }
}

void draw_frame(surface_t *s, int x, int y, int w, int h, uint32_t c) {
    draw_fill(s, x, y, w, 1, c);
    draw_fill(s, x, y + h - 1, w, 1, c);
    draw_fill(s, x, y, 1, h, c);
    draw_fill(s, x + w - 1, y, 1, h, c);
}

/* Coverage (0..255) of pixel (px, py) by a rounded rectangle.  Distances
 * are in 1/16 px; a pixel half inside the curve gets half coverage. */
static int coverage(int px, int py, int x, int y, int w, int h, int r, int corners) {
    int cx, cy, mask, dx, dy, v;
    if (px < x || py < y || px >= x + w || py >= y + h) return 0;
    if (r <= 0) return 255;
    if (px < x + r) {
        cx = x + r;
        mask = py < y + r ? RR_TL : (py >= y + h - r ? RR_BL : 0);
    } else if (px >= x + w - r) {
        cx = x + w - r;
        mask = py < y + r ? RR_TR : (py >= y + h - r ? RR_BR : 0);
    } else {
        return 255;
    }
    if (!(mask & corners)) return 255;
    cy = py < y + r ? y + r : y + h - r;
    dx = cx * 16 - (px * 16 + 8);
    dy = cy * 16 - (py * 16 + 8);
    v = 128 + (r * 16 - (int)isqrt((uint32_t)(dx * dx + dy * dy))) * 16;
    return v < 0 ? 0 : (v > 255 ? 255 : v);
}

void draw_rrect(surface_t *s, int x, int y, int w, int h, int r, int corners, uint32_t c, uint32_t a) {
    int x0 = x, y0 = y, cw = w, ch = h;
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;
    if (!a || !clip(s, &x0, &y0, &cw, &ch)) return;
    for (int py = y0; py < y0 + ch; py++) {
        int edge = py < y + r || py >= y + h - r;
        uint32_t *row = s->px + py * s->stride;
        for (int px = x0; px < x0 + cw; px++) {
            if (edge && (px < x + r || px >= x + w - r)) {
                int cov = coverage(px, py, x, y, w, h, r, corners);
                if (cov) put(s, px, py, c, a * (uint32_t)cov / 255);
            } else {
                row[px] = a >= 255 ? c : blend(row[px], c, a);
            }
        }
    }
}

void draw_round_fill(surface_t *s, int x, int y, int w, int h, int r, uint32_t c) {
    draw_rrect(s, x, y, w, h, r, RR_ALL, c, 255);
}

void draw_circle(surface_t *s, int cx, int cy, int r, uint32_t c, uint32_t a) {
    draw_rrect(s, cx - r, cy - r, 2 * r, 2 * r, r, RR_ALL, c, a);
}

void draw_rrect_outline(surface_t *s, int x, int y, int w, int h, int r, uint32_t c, uint32_t a) {
    rect_t all = { 0, 0, s->w, s->h }, box = { x, y, w, h }, cl;
    if (!rect_intersect(box, all, &cl)) return;
    for (int py = cl.y; py < cl.y + cl.h; py++) {
        int band = py < y + r + 1 || py >= y + h - r - 1;
        for (int px = cl.x; px < cl.x + cl.w; px++) {
            int ring;
            if (!band && px > x + r && px < x + w - r - 1) {
                px = x + w - r - 2; /* skip the interior of middle rows */
                continue;
            }
            ring = coverage(px, py, x, y, w, h, r, RR_ALL) -
                   coverage(px, py, x + 1, y + 1, w - 2, h - 2, r > 1 ? r - 1 : 0, RR_ALL);
            if (ring > 0) put(s, px, py, c, a * (uint32_t)ring / 255);
        }
    }
}

void draw_shadow(surface_t *s, int x, int y, int w, int h, int r, int size, uint32_t c, uint32_t a) {
    rect_t all = { 0, 0, s->w, s->h }, box = { x - size, y - size, w + 2 * size, h + 2 * size }, cl;
    int s16 = size * 16;
    if (!rect_intersect(box, all, &cl)) return;
    for (int py = cl.y; py < cl.y + cl.h; py++) {
        int pcy = py * 16 + 8, qy;
        qy = imax(imax((y + r) * 16 - pcy, pcy - (y + h - r) * 16), 0);
        for (int px = cl.x; px < cl.x + cl.w; px++) {
            int pcx = px * 16 + 8, qx, d, u;
            if (qy == 0 && px >= x + r && px < x + w - r) {
                px = x + w - r - 1; /* interior is covered by the window */
                continue;
            }
            qx = imax(imax((x + r) * 16 - pcx, pcx - (x + w - r) * 16), 0);
            d = (qx && qy) ? (int)isqrt((uint32_t)(qx * qx + qy * qy)) : imax(qx, qy);
            d -= r * 16;
            if (d < 0) d = 0;
            u = s16 - d;
            if (u <= 0) continue;
            put(s, px, py, c, a * (uint32_t)u / (uint32_t)s16 * (uint32_t)u / (uint32_t)s16);
        }
    }
}

static uint32_t mix(uint32_t a, uint32_t b, int t, int n) {
    return blend(a, b, (uint32_t)(t * 255 / n));
}

void draw_vgradient(surface_t *s, int x, int y, int w, int h, uint32_t top, uint32_t bottom) {
    for (int j = 0; j < h; j++) draw_fill(s, x, y + j, w, 1, mix(top, bottom, j, h > 1 ? h - 1 : 1));
}

void draw_image(surface_t *s, int x, int y, const uint32_t *px, int w, int h, int stride) {
    for (int j = imax(0, -y); j < h && y + j < s->h; j++) {
        uint32_t *row = s->px + (y + j) * s->stride;
        const uint32_t *src = px + j * stride;
        for (int i = imax(0, -x); i < w && x + i < s->w; i++) {
            uint32_t p = src[i], a = p >> 24, d, ia;
            if (!a) continue;
            if (a == 255) {
                row[x + i] = p & 0xffffffu;
                continue;
            }
            d = row[x + i];
            ia = 256 - (a + (a >> 7));
            row[x + i] = ((p & 0xff00ffu) + (((d & 0xff00ffu) * ia >> 8) & 0xff00ffu)) |
                         ((p & 0x00ff00u) + (((d & 0x00ff00u) * ia >> 8) & 0x00ff00u));
        }
    }
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

/* ---- text -------------------------------------------------------------- */

static const glyph_t *const faces[3] = { font_sans_glyphs, font_bold_glyphs, font_mono_glyphs };
static const uint8_t *const bits[3] = { font_sans_bits, font_bold_bits, font_mono_bits };

/* Decode one UTF-8 code point; advances *p.  Invalid bytes become '?'. */
static uint32_t utf8_next(const char **p) {
    const unsigned char *s = (const unsigned char *)*p;
    uint32_t cp = '?';
    int n = 1;
    if (s[0] < 0x80) cp = s[0];
    else if ((s[0] & 0xe0) == 0xc0 && (s[1] & 0xc0) == 0x80) cp = ((s[0] & 0x1fu) << 6) | (s[1] & 0x3fu), n = 2;
    else if ((s[0] & 0xf0) == 0xe0 && (s[1] & 0xc0) == 0x80 && (s[2] & 0xc0) == 0x80)
        cp = ((s[0] & 0x0fu) << 12) | ((s[1] & 0x3fu) << 6) | (s[2] & 0x3fu), n = 3;
    *p += n;
    return cp;
}

static const glyph_t *glyph(uint32_t cp, int font) {
    const glyph_t *g = faces[font];
    if (cp >= 32 && cp < 127) return &g[cp - 32];
    for (int i = 95; i < FONT_GLYPHS; i++) {
        if (g[i].cp == cp) return &g[i];
    }
    return &g['?' - 32];
}

static void draw_glyph(surface_t *s, int x, int y, const glyph_t *g, int font, uint32_t fg) {
    const uint8_t *b = bits[font] + g->off;
    int gx = x + g->xo, gy = y + g->yo;
    if (gx >= s->w || gy >= s->h || gx + g->w <= 0 || gy + g->h <= 0) return;
    for (int j = 0; j < g->h; j++) {
        int py = gy + j;
        if (py < 0 || py >= s->h) continue;
        for (int i = 0; i < g->w; i++) {
            int px = gx + i;
            uint32_t a = b[j * g->w + i];
            if (a && px >= 0 && px < s->w) put(s, px, py, fg, a);
        }
    }
}

int draw_text_f(surface_t *s, int x, int y, const char *utf8, uint32_t fg, int font) {
    int x0 = x;
    while (*utf8) {
        const glyph_t *g = glyph(utf8_next(&utf8), font);
        draw_glyph(s, x, y, g, font, fg);
        x += g->adv;
    }
    return x - x0;
}

int text_width_n(const char *utf8, int bytes, int font) {
    const char *end = utf8 + bytes;
    int w = 0;
    while (*utf8 && utf8 < end) w += glyph(utf8_next(&utf8), font)->adv;
    return w;
}

int text_width_f(const char *utf8, int font) {
    int w = 0;
    while (*utf8) w += glyph(utf8_next(&utf8), font)->adv;
    return w;
}

void draw_text_clip_f(surface_t *s, int x, int y, const char *utf8, uint32_t fg, int max_px, int font) {
    const glyph_t *dots = glyph(0x2026, font);
    int w = 0;
    if (text_width_f(utf8, font) <= max_px) {
        draw_text_f(s, x, y, utf8, fg, font);
        return;
    }
    while (*utf8) {
        const char *p = utf8;
        const glyph_t *g = glyph(utf8_next(&p), font);
        if (w + g->adv + dots->adv > max_px) break;
        draw_glyph(s, x + w, y, g, font, fg);
        w += g->adv;
        utf8 = p;
    }
    draw_glyph(s, x + w, y, dots, font, fg);
}

int draw_text(surface_t *s, int x, int y, const char *text, uint32_t fg) {
    return draw_text_f(s, x, y, text, fg, FONT_SANS);
}

int text_width(const char *text) {
    return text_width_f(text, FONT_SANS);
}

void draw_text_clip(surface_t *s, int x, int y, const char *text, uint32_t fg, int max_px) {
    draw_text_clip_f(s, x, y, text, fg, max_px, FONT_SANS);
}

void draw_char(surface_t *s, int x, int y, char ch, uint32_t fg) {
    unsigned char c = (unsigned char)ch;
    draw_glyph(s, x, y, glyph(c >= 32 && c < 127 ? c : '?', FONT_MONO), FONT_MONO, fg);
}

/* ---- icons and cursor -------------------------------------------------- */

void draw_icon(surface_t *s, int x, int y, int icon) {
    switch (icon) {
    case ICON_FOLDER: draw_image(s, x, y, icon_folder, 48, 48, 48); break;
    case ICON_FILE: draw_image(s, x, y, icon_file, 48, 48, 48); break;
    case ICON_APP: draw_image(s, x, y, icon_app, 48, 48, 48); break;
    case ICON_TERMINAL: draw_image(s, x, y, icon_terminal, 48, 48, 48); break;
    case ICON_POWER: draw_image(s, x, y, icon_power, 16, 16, 16); break;
    }
}

void draw_cursor(surface_t *s, int x, int y) {
    draw_image(s, x, y, cursor_img, CURSOR_W, CURSOR_H, CURSOR_W);
}

int cursor_width(void) { return CURSOR_W; }
int cursor_height(void) { return CURSOR_H; }

/* ---- widgets ----------------------------------------------------------- */

void ui_button(surface_t *s, int x, int y, int w, int h, const char *label, int active, int hover) {
    uint32_t fill = active ? COL_ACCENT : (hover ? COL_PANEL_HI : COL_PANEL);
    uint32_t fg = active ? 0x0b1117u : COL_TEXT;
    int tw = text_width(label);
    draw_rrect(s, x, y, w, h, 6, RR_ALL, fill, 255);
    if (!active) draw_rrect_outline(s, x, y, w, h, 6, hover ? 0x3b4759u : COL_EDGE, 255);
    draw_text_clip(s, x + (w - tw > 8 ? (w - tw) / 2 : 4), y + (h - GLYPH_H) / 2, label, fg, w - 8);
}

#define MENU_ITEM_H 26
#define MENU_PAD 5

int menu_width(const menu_t *m) {
    int w = 0;
    for (int i = 0; i < m->count; i++) {
        int t = text_width(m->items[i]);
        if (t > w) w = t;
    }
    return w + 40;
}

int menu_height(const menu_t *m) {
    return m->count * MENU_ITEM_H + 2 * MENU_PAD;
}

rect_t menu_bounds(const menu_t *m, int x, int y) {
    rect_t r = { x - 14, y - 10, menu_width(m) + 28, menu_height(m) + 28 };
    return r;
}

void menu_draw(surface_t *s, int x, int y, const menu_t *m, int hover) {
    int w = menu_width(m), h = menu_height(m);
    draw_shadow(s, x, y + 4, w, h, 8, 12, 0x000000, 140);
    draw_rrect(s, x, y, w, h, 8, RR_ALL, COL_PANEL, 250);
    draw_rrect_outline(s, x, y, w, h, 8, 0x34404f, 255);
    for (int i = 0; i < m->count; i++) {
        int iy = y + MENU_PAD + i * MENU_ITEM_H;
        if (i == hover) draw_rrect(s, x + 5, iy + 1, w - 10, MENU_ITEM_H - 2, 5, RR_ALL, COL_SELECT, 255);
        draw_text(s, x + 16, iy + (MENU_ITEM_H - GLYPH_H) / 2, m->items[i], COL_TEXT);
    }
}

int menu_hit(const menu_t *m, int x, int y, int mx, int my) {
    int w = menu_width(m);
    if (mx < x || mx >= x + w || my < y + MENU_PAD) return -1;
    int i = (my - y - MENU_PAD) / MENU_ITEM_H;
    return i < m->count ? i : -1;
}
