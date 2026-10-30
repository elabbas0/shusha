/* Shusha window manager / compositor.
 *
 * Every change becomes a damage rectangle; each frame composites only
 * the damaged rectangles (through a clipped view of a RAM back buffer)
 * and copies only those to the framebuffer.  On top of that:
 *  - wallpaper: solid colour with the Xarıbülbül emblem, built once;
 *  - alpha compositing: soft window shadows, anti-aliased rounded
 *    corners, translucent top bar (blurred wallpaper) and dock;
 *  - animations: windows fade/slide in, out, and to the dock; while a
 *    window animates it is drawn opaque and lerped against a snapshot
 *    of what was beneath it, so only that window's area is redrawn;
 *  - title-bar close/minimise buttons, dock with running indicators.
 */
#include "lib.h"
#include "draw.h"
#include "ui_proto.h"

#define MAX_W     1920
#define MAX_H     1200
#define MAX_WIN   16
#define MAX_DMG   32
#define PANEL_H   30
#define TITLE_H   34
#define RADIUS    10
#define SHADOW    26
#define SHADOW_DY 8
#define DOCK_ICON 48
#define DOCK_GAP  12
#define DOCK_H    (DOCK_ICON + 20)
#define DOCK_MARGIN 10
#define WALL_COLOR 0x11161fu

enum { ANIM_NONE, ANIM_OPEN, ANIM_CLOSE, ANIM_MIN, ANIM_RESTORE };

typedef struct {
    int used;
    uint32_t id;
    uint32_t pid;
    uint64_t q;
    uint64_t shm;
    uint32_t *px;
    int x, y, w, h;        /* content rectangle, screen coordinates */
    int minimized;
    int anim;
    uint64_t anim_t0;
    char title[UI_TITLE_MAX];
} win_t;

extern const uint8_t wallpaper_raw[]; /* u32 w, u32 h, premultiplied ARGB */

static uint32_t backbuf[MAX_W * MAX_H];
static uint32_t wallpaper[MAX_W * MAX_H];
static uint32_t scratch[MAX_W * MAX_H];
static uint32_t panel_bg[MAX_W * PANEL_H];
static surface_t screen;
static sh_fb_info_t fb;
static uint8_t *fbmem;
static int needs_present;

static win_t wins[MAX_WIN];
static int zorder[MAX_WIN]; /* bottom .. top */
static int zcount;
static int focus = -1;
static uint32_t next_id = 1;

static rect_t dmg[MAX_DMG];
static int ndmg;

static int mouse_x, mouse_y;
static uint32_t mouse_buttons;
static int drag = -1, drag_dx, drag_dy;
static int capture = -1;

/* What the pointer is over, for hover feedback. */
enum { HV_NONE, HV_DOCK, HV_POWER, HV_CLOSE, HV_MIN, HV_MENU };
static int hover_kind = HV_NONE, hover_idx = -1;

static int power_open;
static const menu_t power_menu = { { "Restart", "Power off" }, 2 };
static char clock_text[16];

/* ---- geometry ---------------------------------------------------------- */

static rect_t R(int x, int y, int w, int h) {
    rect_t r = { x, y, w, h };
    return r;
}

static uint32_t anim_progress(const win_t *w) {
    uint32_t dur = (w->anim == ANIM_OPEN || w->anim == ANIM_RESTORE) ? 16 : 12;
    uint64_t t = sh_ticks() - w->anim_t0;
    return t >= dur ? 256 : (uint32_t)(t * 256 / dur);
}

/* Opacity 0..255 and vertical offset of a window right now. */
static void anim_state(const win_t *w, uint32_t *alpha, int *dy) {
    uint32_t p = anim_progress(w), e = 256 - (256 - p) * (256 - p) / 256; /* ease-out */
    switch (w->anim) {
    case ANIM_OPEN:
    case ANIM_RESTORE:
        *alpha = e * 255 / 256;
        *dy = (int)((256 - e) * (w->anim == ANIM_OPEN ? 18 : 60) / 256);
        break;
    case ANIM_CLOSE:
        *alpha = (256 - e) * 255 / 256;
        *dy = (int)(e * 10 / 256);
        break;
    case ANIM_MIN:
        *alpha = (256 - e) * 255 / 256;
        *dy = (int)(e * 60 / 256);
        break;
    default:
        *alpha = 255;
        *dy = 0;
    }
}

static int win_visible(const win_t *w) {
    return w->used && (!w->minimized || w->anim == ANIM_RESTORE);
}

static rect_t frame_rect(const win_t *w) {
    return R(w->x, w->y - TITLE_H, w->w, w->h + TITLE_H);
}

/* Everything a window can touch: frame, shadow, animation offsets. */
static rect_t win_bounds(const win_t *w) {
    rect_t f = frame_rect(w);
    return R(f.x - SHADOW, f.y - SHADOW, f.w + 2 * SHADOW, f.h + 2 * SHADOW + SHADOW_DY + 64);
}

static rect_t title_button(const win_t *w, int which, int dy) {
    /* which 0 = close, 1 = minimise; returned as a hit box */
    int cx = w->x + w->w - 22 - which * 24, cy = w->y - TITLE_H / 2 + dy;
    return R(cx - 10, cy - 10, 20, 20);
}

/* ---- damage ------------------------------------------------------------ */

static void damage(rect_t r) {
    rect_t all = { 0, 0, screen.w, screen.h }, c;
    if (!rect_intersect(r, all, &c)) return;
    for (int i = 0; i < ndmg; i++) {
        rect_t grow = { dmg[i].x - 8, dmg[i].y - 8, dmg[i].w + 16, dmg[i].h + 16 }, tmp;
        if (rect_intersect(grow, c, &tmp)) {
            dmg[i] = rect_union(dmg[i], c);
            return;
        }
    }
    if (ndmg == MAX_DMG) {
        for (int i = 1; i < ndmg; i++) dmg[0] = rect_union(dmg[0], dmg[i]);
        ndmg = 1;
        dmg[0] = rect_union(dmg[0], c);
        return;
    }
    dmg[ndmg++] = c;
}

static void damage_window(const win_t *w) {
    damage(win_bounds(w));
}

/* ---- dock -------------------------------------------------------------- */

typedef struct {
    const char *label;
    const char *app;   /* path to launch, or 0 for a plain window */
    int icon;
    int win;           /* window index for plain-window entries */
} dock_item_t;

static dock_item_t dock[2 + MAX_WIN];
static int ndock;

static int title_is(const win_t *w, const char *name) {
    return str_eq(w->title, name);
}

static void dock_build(void) {
    ndock = 0;
    dock[ndock++] = (dock_item_t){ "Explorer", "/apps/explorer.app", ICON_FOLDER, -1 };
    dock[ndock++] = (dock_item_t){ "Terminal", "/apps/terminal.app", ICON_TERMINAL, -1 };
    for (int i = 0; i < MAX_WIN; i++) {
        if (!wins[i].used || title_is(&wins[i], "Explorer") || title_is(&wins[i], "Terminal")) continue;
        dock[ndock++] = (dock_item_t){ wins[i].title, 0, ICON_APP, i };
    }
}

static rect_t dock_rect(void) {
    int w = ndock * (DOCK_ICON + DOCK_GAP) + DOCK_GAP;
    return R((screen.w - w) / 2, screen.h - DOCK_H - DOCK_MARGIN, w, DOCK_H);
}

static rect_t dock_icon_rect(int i) {
    rect_t d = dock_rect();
    return R(d.x + DOCK_GAP + i * (DOCK_ICON + DOCK_GAP), d.y + 8, DOCK_ICON, DOCK_ICON);
}

/* The strip the dock and its tooltips can cover. */
static void damage_dock(void) {
    damage(R(0, screen.h - DOCK_H - DOCK_MARGIN - 40, screen.w, DOCK_H + DOCK_MARGIN + 40));
}

/* 0 = not running, 1 = running, 2 = all of its windows minimised */
static int dock_state(const dock_item_t *d) {
    int any = 0, shown = 0;
    for (int i = 0; i < MAX_WIN; i++) {
        const win_t *w = &wins[i];
        if (!w->used || w->anim == ANIM_CLOSE) continue;
        if (d->win >= 0 ? i != d->win : !title_is(w, d->label)) continue;
        any = 1;
        if (!w->minimized) shown = 1;
    }
    return !any ? 0 : (shown ? 1 : 2);
}

/* ---- panel ------------------------------------------------------------- */

static rect_t power_rect(void) {
    return R(screen.w - 40, 3, 32, PANEL_H - 6);
}

static rect_t clock_rect(void) {
    return R(screen.w - 40 - 120, 0, 112, PANEL_H);
}

static int power_menu_x(void) {
    return screen.w - menu_width(&power_menu) - 8;
}

static void update_clock(void) {
    char t[16], n[8];
    uint64_t s = sh_ticks() / 100;
    t[0] = '\0';
    for (int part = 0; part < 3; part++) {
        uint64_t v = part == 0 ? s / 3600 : part == 1 ? (s / 60) % 60 : s % 60;
        if (v < 10) str_cat(t, "0", sizeof(t));
        str_cat(t, fmt_u64(n, v, 10), sizeof(t));
        if (part < 2) str_cat(t, ":", sizeof(t));
    }
    if (!str_eq(t, clock_text)) {
        str_copy(clock_text, t, sizeof(clock_text));
        damage(clock_rect());
    }
}

/* ---- composition ------------------------------------------------------- */

static void paint_window(surface_t *v, int ox, int oy, const win_t *w, int idx, int dy) {
    int focused = idx == focus;
    int fx = w->x + ox, fy = w->y - TITLE_H + oy + dy, fw = w->w, fh = w->h + TITLE_H;
    uint32_t bar = focused ? 0x1d2531u : 0x161c25u;

    draw_shadow(v, fx, fy + SHADOW_DY, fw, fh, RADIUS, SHADOW, 0x000000, focused ? 150 : 100);
    draw_rrect(v, fx, fy, fw, TITLE_H, RADIUS, RR_TL | RR_TR, bar, 255);
    draw_fill(v, fx, fy + TITLE_H - 1, fw, 1, 0x0c1016);

    {
        int tw = text_width_f(w->title, FONT_BOLD), max = fw - 120;
        int tx = fx + (fw - (tw < max ? tw : max)) / 2;
        draw_text_clip_f(v, tx, fy + (TITLE_H - GLYPH_H) / 2, w->title, focused ? COL_TEXT : COL_MUTED, max, FONT_BOLD);
    }
    for (int b = 0; b < 2; b++) {
        rect_t hb = title_button(w, b, dy);
        int cx = hb.x + 10 + ox, cy = hb.y + 10 + oy;
        int hov = (hover_kind == (b ? HV_MIN : HV_CLOSE)) && hover_idx == idx;
        uint32_t col = focused || hov ? (b ? COL_WARN : COL_DANGER) : 0x3a4452u;
        draw_circle(v, cx, cy, 7, col, 255);
        if (hov) {
            if (b) {
                draw_fill(v, cx - 4, cy - 1, 8, 2, 0x4a3410);
            } else {
                for (int k = -3; k <= 3; k++) {
                    draw_fill_a(v, cx + k, cy + k, 1, 1, 0x4a1410, 255);
                    draw_fill_a(v, cx + k, cy - k, 1, 1, 0x4a1410, 255);
                    draw_fill_a(v, cx + k + 1, cy + k, 1, 1, 0x4a1410, 120);
                    draw_fill_a(v, cx + k + 1, cy - k, 1, 1, 0x4a1410, 120);
                }
            }
        }
    }

    /* Content, with the bottom corners rounded by coverage masks. */
    {
        int cy0 = fy + TITLE_H;
        draw_blit(v, fx, cy0, w->px, w->w, w->w, w->h - RADIUS);
        for (int j = w->h - RADIUS; j < w->h; j++) {
            int py = cy0 + j;
            if (py < 0 || py >= v->h) continue;
            for (int i = 0; i < w->w; i++) {
                int px = fx + i, d, cov;
                uint32_t *p;
                if (px < 0 || px >= v->w) continue;
                p = v->px + py * v->stride + px;
                if (i >= RADIUS && i < w->w - RADIUS) {
                    *p = w->px[j * w->w + i];
                    continue;
                }
                {
                    int ccx = (i < RADIUS ? RADIUS : w->w - RADIUS) * 16, ccy = (w->h - RADIUS) * 16;
                    int dx = ccx - (i * 16 + 8), dyy = ccy - (j * 16 + 8);
                    uint32_t q = (uint32_t)(dx * dx + dyy * dyy), r = 0, bit = 1u << 30;
                    while (bit > q) bit >>= 2;
                    while (bit) {
                        if (q >= r + bit) { q -= r + bit; r = (r >> 1) + bit; } else r >>= 1;
                        bit >>= 2;
                    }
                    d = (int)r;
                }
                cov = 128 + (RADIUS * 16 - d) * 16;
                if (cov <= 0) continue;
                *p = cov >= 255 ? w->px[j * w->w + i] : blend(*p, w->px[j * w->w + i], (uint32_t)cov);
            }
        }
    }
    draw_rrect_outline(v, fx, fy, fw, fh, RADIUS, 0xffffff, focused ? 34 : 20);
}

static void paint_dock(surface_t *v, int ox, int oy) {
    rect_t d = dock_rect();
    draw_shadow(v, d.x + ox, d.y + oy + 6, d.w, d.h, 18, 18, 0x000000, 110);
    draw_rrect(v, d.x + ox, d.y + oy, d.w, d.h, 18, RR_ALL, 0x0b0f15, 196);
    draw_rrect_outline(v, d.x + ox, d.y + oy, d.w, d.h, 18, 0xffffff, 26);
    for (int i = 0; i < ndock; i++) {
        rect_t ic = dock_icon_rect(i);
        int st = dock_state(&dock[i]), hov = hover_kind == HV_DOCK && hover_idx == i;
        if (hov) draw_rrect(v, ic.x - 5 + ox, ic.y - 5 + oy, ic.w + 10, ic.h + 10, 12, RR_ALL, 0xffffff, 30);
        draw_icon(v, ic.x + ox, ic.y + oy - (hov ? 2 : 0), dock[i].icon);
        if (st) draw_circle(v, ic.x + ic.w / 2 + ox, ic.y + ic.h + 6 + oy, 2, st == 1 ? COL_ACCENT_HI : COL_MUTED, 255);
        if (hov) {
            int tw = text_width(dock[i].label) + 20, tx = ic.x + ic.w / 2 - tw / 2, ty = d.y - 34;
            draw_shadow(v, tx + ox, ty + oy + 3, tw, 24, 8, 8, 0x000000, 120);
            draw_rrect(v, tx + ox, ty + oy, tw, 24, 8, RR_ALL, 0x1d2531, 245);
            draw_text(v, tx + 10 + ox, ty + 4 + oy, dock[i].label, COL_TEXT);
        }
    }
}

static void paint_panel(surface_t *v, int ox, int oy) {
    draw_blit(v, ox, oy, panel_bg, screen.w, screen.w, PANEL_H);
    draw_fill_a(v, ox, PANEL_H - 1 + oy, screen.w, 1, 0xffffff, 22);
    draw_circle(v, 18 + ox, PANEL_H / 2 + oy, 5, COL_ACCENT_HI, 255);
    draw_text_f(v, 30 + ox, 7 + oy, "Shusha", COL_TEXT, FONT_BOLD);
    if (focus >= 0 && wins[focus].used && !wins[focus].minimized) {
        draw_text_clip(v, 30 + text_width_f("Shusha", FONT_BOLD) + 18 + ox, 7 + oy, wins[focus].title, COL_MUTED, 240);
    }
    {
        rect_t c = clock_rect();
        draw_text_f(v, c.x + c.w - text_width_f(clock_text, FONT_MONO) + ox, 7 + oy, clock_text, 0xb8c2cf, FONT_MONO);
    }
    {
        rect_t p = power_rect();
        if (hover_kind == HV_POWER || power_open) draw_rrect(v, p.x + ox, p.y + oy, p.w, p.h, 6, RR_ALL, 0xffffff, 30);
        draw_icon(v, p.x + 8 + ox, p.y + (p.h - 16) / 2 + oy, ICON_POWER);
    }
}

static void composite(rect_t r) {
    surface_t v = { backbuf + r.y * screen.stride + r.x, r.w, r.h, screen.stride };
    int ox = -r.x, oy = -r.y;

    for (int j = 0; j < r.h; j++) {
        const uint32_t *s = wallpaper + (r.y + j) * screen.stride + r.x;
        uint32_t *d = v.px + j * v.stride;
        for (int i = 0; i < r.w; i++) d[i] = s[i];
    }

    for (int k = 0; k < zcount; k++) {
        int idx = zorder[k], dy;
        uint32_t alpha;
        rect_t b, part;
        const win_t *w = &wins[idx];
        if (!win_visible(w) && w->anim != ANIM_MIN && w->anim != ANIM_CLOSE) continue;
        b = win_bounds(w);
        if (!rect_intersect(b, r, &part)) continue;
        anim_state(w, &alpha, &dy);
        if (alpha == 0) continue;
        if (alpha < 255) {
            for (int j = 0; j < part.h; j++) {
                mem_copy(scratch + j * part.w, backbuf + (part.y + j) * screen.stride + part.x, (uint64_t)part.w * 4);
            }
        }
        paint_window(&v, ox, oy, w, idx, dy);
        if (alpha < 255) {
            for (int j = 0; j < part.h; j++) {
                uint32_t *d = backbuf + (part.y + j) * screen.stride + part.x;
                const uint32_t *s = scratch + j * part.w;
                for (int i = 0; i < part.w; i++) d[i] = blend(s[i], d[i], alpha);
            }
        }
    }

    {
        rect_t strip = { 0, screen.h - DOCK_H - DOCK_MARGIN - 40, screen.w, DOCK_H + DOCK_MARGIN + 40 }, tmp;
        if (rect_intersect(strip, r, &tmp)) paint_dock(&v, ox, oy);
    }
    if (r.y < PANEL_H) paint_panel(&v, ox, oy);
    if (power_open) {
        rect_t mb = menu_bounds(&power_menu, power_menu_x(), PANEL_H + 4), tmp;
        if (rect_intersect(mb, r, &tmp)) {
            menu_draw(&v, power_menu_x() + ox, PANEL_H + 4 + oy, &power_menu,
                      hover_kind == HV_MENU ? hover_idx : -1);
        }
    }
    draw_cursor(&v, mouse_x - 1 + ox, mouse_y - 1 + oy);
}

static void upload(rect_t r) {
    for (int j = 0; j < r.h; j++) {
        const uint32_t *s = backbuf + (r.y + j) * screen.stride + r.x;
        uint8_t *d = fbmem + (uint64_t)(r.y + j) * fb.pitch;
        if (fb.bpp == 32) {
            uint32_t *d32 = (uint32_t *)d + r.x;
            for (int i = 0; i < r.w; i++) d32[i] = s[i];
        } else {
            d += r.x * 3;
            for (int i = 0; i < r.w; i++) {
                d[0] = (uint8_t)s[i];
                d[1] = (uint8_t)(s[i] >> 8);
                d[2] = (uint8_t)(s[i] >> 16);
                d += 3;
            }
        }
    }
}

static void flush_damage(void) {
    if (!ndmg) return;
    for (int i = 0; i < ndmg; i++) composite(dmg[i]);
    for (int i = 0; i < ndmg; i++) upload(dmg[i]);
    ndmg = 0;
    if (needs_present) sh_gpu_present();
}

/* ---- wallpaper --------------------------------------------------------- */

/* Solid colour plus the Xarıbülbül emblem, scaled (bilinear, on
 * premultiplied pixels) and centred on the right of the screen. */
static void build_wallpaper(void) {
    const uint32_t *hdr = (const uint32_t *)wallpaper_raw;
    const uint32_t *art = hdr + 2;
    int aw = (int)hdr[0], ah = (int)hdr[1];
    int th = screen.h * 64 / 100, tw = aw * th / ah;
    int x0 = screen.w * 80 / 100 - tw / 2, y0 = (screen.h - th) / 2 + PANEL_H / 2 - 24;
    uint32_t step = (uint32_t)(((uint64_t)ah << 16) / (uint64_t)th);

    for (int i = 0; i < screen.w * screen.h; i++) wallpaper[i] = WALL_COLOR;
    if (x0 + tw > screen.w - 10) x0 = screen.w - 10 - tw;

    for (int y = 0; y < th; y++) {
        uint32_t sy = (uint32_t)y * step, fy = (sy >> 8) & 255;
        int iy = (int)(sy >> 16);
        if (iy >= ah - 1) iy = ah - 2, fy = 255;
        for (int x = 0; x < tw; x++) {
            uint32_t sx = (uint32_t)x * step, fx = (sx >> 8) & 255, p[4], out = 0;
            int ix = (int)(sx >> 16), px = x0 + x, py = y0 + y;
            if (px < 0 || px >= screen.w || py < 0 || py >= screen.h) continue;
            if (ix >= aw - 1) ix = aw - 2, fx = 255;
            p[0] = art[iy * aw + ix];
            p[1] = art[iy * aw + ix + 1];
            p[2] = art[(iy + 1) * aw + ix];
            p[3] = art[(iy + 1) * aw + ix + 1];
            if (!(p[0] | p[1] | p[2] | p[3])) continue;
            for (int sh = 0; sh < 32; sh += 8) {
                uint32_t a = (p[0] >> sh) & 255, b = (p[1] >> sh) & 255, c = (p[2] >> sh) & 255, d = (p[3] >> sh) & 255;
                uint32_t top = a * (256 - fx) + b * fx, bot = c * (256 - fx) + d * fx;
                out |= (((top * (256 - fy) + bot * fy) >> 16) & 255) << sh;
            }
            {
                uint32_t a = out >> 24, *dst = &wallpaper[py * screen.stride + px], ia = 256 - (a + (a >> 7));
                *dst = ((out & 0xff00ffu) + (((*dst & 0xff00ffu) * ia >> 8) & 0xff00ffu)) |
                       ((out & 0x00ff00u) + (((*dst & 0x00ff00u) * ia >> 8) & 0x00ff00u));
            }
        }
    }

    /* Top bar background: horizontally blurred wallpaper under a tint. */
    for (int y = 0; y < PANEL_H; y++) {
        const uint32_t *src = wallpaper + y * screen.stride;
        uint32_t *dst = panel_bg + y * screen.w;
        for (int x = 0; x < screen.w; x++) {
            uint32_t r = 0, g = 0, b = 0, n = 0;
            for (int k = x - 12; k <= x + 12; k += 3) {
                uint32_t c;
                if (k < 0 || k >= screen.w) continue;
                c = src[k];
                r += (c >> 16) & 255;
                g += (c >> 8) & 255;
                b += c & 255;
                n++;
            }
            dst[x] = blend(((r / n) << 16) | ((g / n) << 8) | (b / n), 0x0a0e14, 190);
        }
    }
}

/* ---- windows ----------------------------------------------------------- */

static void send(win_t *w, ui_msg_t *m) {
    m->window = w->id;
    (void)sh_msg_send(w->q, m);
}

static void set_focus(int idx) {
    ui_msg_t m;
    if (idx == focus) return;
    if (focus >= 0 && wins[focus].used) {
        mem_set(&m, 0, sizeof(m));
        m.type = UI_FOCUS;
        send(&wins[focus], &m);
        damage(frame_rect(&wins[focus]));
    }
    focus = idx;
    damage(R(0, 0, screen.w, PANEL_H));
    if (idx >= 0) {
        int k, found = 0;
        for (k = 0; k < zcount; k++) {
            if (zorder[k] == idx) found = 1;
            if (found && k + 1 < zcount) zorder[k] = zorder[k + 1];
        }
        zorder[zcount - 1] = idx;
        mem_set(&m, 0, sizeof(m));
        m.type = UI_FOCUS;
        m.focus.focused = 1;
        send(&wins[idx], &m);
        damage_window(&wins[idx]);
    }
}

/* Focus the topmost window that is shown and not leaving. */
static void focus_next(void) {
    for (int k = zcount - 1; k >= 0; k--) {
        const win_t *w = &wins[zorder[k]];
        if (w->used && !w->minimized && w->anim != ANIM_CLOSE && w->anim != ANIM_MIN) {
            set_focus(zorder[k]);
            return;
        }
    }
    focus = -1;
    damage(R(0, 0, screen.w, PANEL_H));
}

static int win_by_id(uint32_t id) {
    for (int i = 0; i < MAX_WIN; i++) {
        if (wins[i].used && wins[i].id == id) return i;
    }
    return -1;
}

static void start_anim(int idx, int kind) {
    damage_window(&wins[idx]);
    wins[idx].anim = kind;
    wins[idx].anim_t0 = sh_ticks();
}

static void free_window(int idx) {
    win_t *w = &wins[idx];
    int k, j = 0;
    damage_window(w);
    sh_shm_unmap(w->shm);
    sh_shm_close(w->shm);
    w->used = 0;
    for (k = 0; k < zcount; k++) {
        if (zorder[k] != idx) zorder[j++] = zorder[k];
    }
    zcount = j;
    dock_build();
    damage_dock();
}

static void close_window(int idx) {
    win_t *w = &wins[idx];
    if (w->anim == ANIM_CLOSE) return;
    if (drag == idx) drag = -1;
    if (capture == idx) capture = -1;
    if (w->minimized && w->anim != ANIM_RESTORE) {
        free_window(idx);
        return;
    }
    w->minimized = 0;
    start_anim(idx, ANIM_CLOSE);
    if (focus == idx) focus_next();
    damage_dock();
}

static void minimize_window(int idx) {
    if (drag == idx) drag = -1;
    if (capture == idx) capture = -1;
    wins[idx].minimized = 1;
    start_anim(idx, ANIM_MIN);
    if (focus == idx) focus_next();
    damage_dock();
}

static void restore_window(int idx) {
    wins[idx].minimized = 0;
    start_anim(idx, ANIM_RESTORE);
    set_focus(idx);
    damage_dock();
}

static void open_window(const ui_msg_t *req, uint32_t pid) {
    char qname[48];
    ui_msg_t m;
    int idx = -1, w = req->open.w, h = req->open.h;
    int max_h = screen.h - PANEL_H - TITLE_H - DOCK_H - DOCK_MARGIN - 24;
    uint64_t q;
    static int cascade;

    /* The kernel-stamped sender, not the pid the request claims. */
    ui_mailbox_name(qname, sizeof(qname), pid);
    q = sh_msg_open(qname);
    if (!q) return;
    mem_set(&m, 0, sizeof(m));
    for (int i = 0; i < MAX_WIN; i++) {
        if (!wins[i].used) {
            idx = i;
            break;
        }
    }
    if (w > screen.w - 40) w = screen.w - 40;
    if (h > max_h) h = max_h;
    if (idx < 0 || w < 32 || h < 32) {
        m.type = UI_OPEN_FAIL;
        (void)sh_msg_send(q, &m);
        return;
    }

    win_t *win = &wins[idx];
    mem_set(win, 0, sizeof(*win));
    win->shm = sh_shm_create((uint64_t)w * (uint64_t)h * 4);
    win->px = win->shm ? sh_shm_map(win->shm) : 0;
    if (!win->px) {
        if (win->shm) sh_shm_close(win->shm);
        m.type = UI_OPEN_FAIL;
        (void)sh_msg_send(q, &m);
        return;
    }
    for (int i = 0; i < w * h; i++) win->px[i] = COL_BG;

    win->used = 1;
    win->id = next_id++;
    win->pid = pid;
    win->q = q;
    win->w = w;
    win->h = h;
    /* Left of centre, so the wallpaper artwork on the right stays visible. */
    win->x = 24 + (cascade % 6) * 36;
    win->y = PANEL_H + TITLE_H + 22 + (cascade % 6) * 30;
    cascade++;
    if (win->x < 12) win->x = 12;
    if (win->x + w > screen.w - 12) win->x = screen.w - 12 - w;
    if (win->y + h > screen.h - DOCK_H - DOCK_MARGIN - 12) win->y = PANEL_H + TITLE_H + 12;
    str_copy(win->title, req->open.title, UI_TITLE_MAX);
    zorder[zcount++] = idx;

    m.type = UI_OPEN_OK;
    m.open_ok.shm = win->shm;
    m.open_ok.w = w;
    m.open_ok.h = h;
    send(win, &m);
    start_anim(idx, ANIM_OPEN);
    set_focus(idx);
    dock_build();
    damage_dock();
}

static void move_window(int idx, int x, int y) {
    win_t *w = &wins[idx];
    if (y - TITLE_H < PANEL_H) y = PANEL_H + TITLE_H;
    if (y > screen.h - 8) y = screen.h - 8;
    if (x + w->w < 60) x = 60 - w->w;
    if (x > screen.w - 60) x = screen.w - 60;
    if (x == w->x && y == w->y) return;
    damage_window(w);
    w->x = x;
    w->y = y;
    damage_window(w);
}

static void reap_dead_windows(void) {
    for (int i = 0; i < MAX_WIN; i++) {
        sh_proc_info_t info;
        if (!wins[i].used || wins[i].anim == ANIM_CLOSE) continue;
        if (sh_proc_info(wins[i].pid, &info) != 0 || info.state == SH_PROC_EXITED) close_window(i);
    }
}

/* Advance animations; returns 1 while any is running. */
static int step_animations(void) {
    int running = 0;
    for (int i = 0; i < MAX_WIN; i++) {
        win_t *w = &wins[i];
        if (!w->used || w->anim == ANIM_NONE) continue;
        damage_window(w);
        if (anim_progress(w) < 256) {
            running = 1;
            continue;
        }
        if (w->anim == ANIM_CLOSE) free_window(i);
        else w->anim = ANIM_NONE;
    }
    return running;
}

/* ---- input ------------------------------------------------------------- */

static int window_at(int x, int y) {
    for (int k = zcount - 1; k >= 0; k--) {
        const win_t *w = &wins[zorder[k]];
        if (!w->used || w->minimized || w->anim == ANIM_CLOSE) continue;
        if (rect_contains(frame_rect(w), x, y)) return zorder[k];
    }
    return -1;
}

static void send_mouse(int idx) {
    ui_msg_t m;
    mem_set(&m, 0, sizeof(m));
    m.type = UI_MOUSE;
    m.mouse.x = mouse_x - wins[idx].x;
    m.mouse.y = mouse_y - wins[idx].y;
    m.mouse.buttons = mouse_buttons;
    send(&wins[idx], &m);
}

static void dock_click(int i) {
    const dock_item_t *d = &dock[i];
    int top = -1;
    for (int k = zcount - 1; k >= 0 && top < 0; k--) {
        const win_t *w = &wins[zorder[k]];
        if (!w->used || w->anim == ANIM_CLOSE) continue;
        if (d->win >= 0 ? zorder[k] == d->win : title_is(w, d->label)) top = zorder[k];
    }
    if (top < 0) {
        if (d->app) (void)sh_spawn(d->app);
        return;
    }
    for (int k = zcount - 1; k >= 0; k--) { /* bring back a minimised one first */
        int idx = zorder[k];
        const win_t *w = &wins[idx];
        if (w->used && w->minimized && w->anim != ANIM_CLOSE &&
            (d->win >= 0 ? idx == d->win : title_is(w, d->label))) {
            restore_window(idx);
            return;
        }
    }
    set_focus(top);
}

static void set_hover(int kind, int idx) {
    if (kind == hover_kind && idx == hover_idx) return;
    for (int pass = 0; pass < 2; pass++) {
        int k = pass ? kind : hover_kind, i = pass ? idx : hover_idx;
        if (k == HV_DOCK) damage_dock();
        else if (k == HV_POWER) damage(power_rect());
        else if ((k == HV_CLOSE || k == HV_MIN) && i >= 0 && wins[i].used) damage(frame_rect(&wins[i]));
        else if (k == HV_MENU) damage(menu_bounds(&power_menu, power_menu_x(), PANEL_H + 4));
    }
    hover_kind = kind;
    hover_idx = idx;
}

static void update_hover(void) {
    if (power_open) {
        int i = menu_hit(&power_menu, power_menu_x(), PANEL_H + 4, mouse_x, mouse_y);
        if (i >= 0) {
            set_hover(HV_MENU, i);
            return;
        }
    }
    if (rect_contains(power_rect(), mouse_x, mouse_y)) {
        set_hover(HV_POWER, 0);
        return;
    }
    for (int i = 0; i < ndock; i++) {
        rect_t ic = dock_icon_rect(i);
        if (rect_contains(R(ic.x - 6, ic.y - 6, ic.w + 12, ic.h + 12), mouse_x, mouse_y)) {
            set_hover(HV_DOCK, i);
            return;
        }
    }
    {
        int idx = window_at(mouse_x, mouse_y);
        if (idx >= 0 && mouse_y < wins[idx].y) {
            for (int b = 0; b < 2; b++) {
                if (rect_contains(title_button(&wins[idx], b, 0), mouse_x, mouse_y)) {
                    set_hover(b ? HV_MIN : HV_CLOSE, idx);
                    return;
                }
            }
        }
    }
    set_hover(HV_NONE, -1);
}

static void mouse_event(const sh_mouse_t *e) {
    uint32_t prev = mouse_buttons;
    int moved = e->abs_x != mouse_x || e->abs_y != mouse_y;
    uint32_t pressed, released;
    rect_t cur = { mouse_x - 2, mouse_y - 2, cursor_width() + 2, cursor_height() + 2 };

    if (moved) damage(cur);
    mouse_x = e->abs_x < 0 ? 0 : (e->abs_x >= screen.w ? screen.w - 1 : e->abs_x);
    mouse_y = e->abs_y < 0 ? 0 : (e->abs_y >= screen.h ? screen.h - 1 : e->abs_y);
    mouse_buttons = e->buttons & 7;
    if (moved) damage(R(mouse_x - 2, mouse_y - 2, cursor_width() + 2, cursor_height() + 2));
    pressed = (mouse_buttons & ~prev) & 7;
    released = (prev & ~mouse_buttons) & 7;

    if (drag >= 0) {
        if (moved) move_window(drag, mouse_x - drag_dx, mouse_y - drag_dy);
        if (released & UI_BTN_LEFT) drag = -1;
        return;
    }
    if (moved) update_hover();

    if (pressed) {
        if (power_open) {
            int i = menu_hit(&power_menu, power_menu_x(), PANEL_H + 4, mouse_x, mouse_y);
            damage(menu_bounds(&power_menu, power_menu_x(), PANEL_H + 4));
            damage(power_rect());
            power_open = 0;
            if (i == 0 || i == 1) {
                (void)sh_sync();
                (void)sh_power(i == 0 ? 1 : 0);
            }
            return;
        }
        if (hover_kind == HV_POWER && (pressed & UI_BTN_LEFT)) {
            power_open = 1;
            damage(menu_bounds(&power_menu, power_menu_x(), PANEL_H + 4));
            return;
        }
        if (hover_kind == HV_DOCK && (pressed & UI_BTN_LEFT)) {
            dock_click(hover_idx);
            return;
        }
        if (mouse_y < PANEL_H) return;
        {
            int idx = window_at(mouse_x, mouse_y);
            win_t *w;
            if (idx < 0) return;
            set_focus(idx);
            w = &wins[idx];
            if (mouse_y < w->y) {
                if (pressed & UI_BTN_LEFT) {
                    if (hover_kind == HV_CLOSE && hover_idx == idx) {
                        ui_msg_t m;
                        mem_set(&m, 0, sizeof(m));
                        m.type = UI_CLOSE;
                        send(w, &m);
                    } else if (hover_kind == HV_MIN && hover_idx == idx) {
                        minimize_window(idx);
                        set_hover(HV_NONE, -1);
                    } else {
                        drag = idx;
                        drag_dx = mouse_x - w->x;
                        drag_dy = mouse_y - w->y;
                    }
                }
                return;
            }
            capture = idx;
            send_mouse(idx);
        }
        return;
    }

    if (capture >= 0) {
        send_mouse(capture);
        if (!mouse_buttons) capture = -1;
        return;
    }
    if (moved && focus >= 0) {
        win_t *w = &wins[focus];
        rect_t content = { w->x, w->y, w->w, w->h };
        if (!w->minimized && rect_contains(content, mouse_x, mouse_y)) send_mouse(focus);
    }
}

static int pump_input(void) {
    sh_mouse_t e;
    long c;
    int busy = 0;

    while (sh_read_mouse(&e) == 0) {
        mouse_event(&e);
        busy = 1;
    }
    while ((c = sh_read_char()) >= 0) {
        busy = 1;
        if (focus >= 0 && !wins[focus].minimized) {
            ui_msg_t m;
            mem_set(&m, 0, sizeof(m));
            m.type = UI_KEY;
            m.key.code = (uint32_t)c;
            send(&wins[focus], &m);
        }
    }
    return busy;
}

static int pump_messages(uint64_t q) {
    ui_msg_t m;
    long from;
    int busy = 0;

    while ((from = sh_msg_recv_from(q, &m)) > 0) {
        int idx;
        busy = 1;
        if (m.type == UI_OPEN) {
            open_window(&m, (uint32_t)from);
            continue;
        }
        idx = win_by_id(m.window);
        /* A window can only be damaged or closed by its own process. */
        if (idx < 0 || wins[idx].pid != (uint32_t)from) continue;
        switch (m.type) {
        case UI_DAMAGE:
            if (!wins[idx].minimized) {
                win_t *w = &wins[idx];
                rect_t r = { w->x + m.damage.x, w->y + m.damage.y, m.damage.w, m.damage.h };
                rect_t content = { w->x, w->y, w->w, w->h }, c;
                if (w->anim != ANIM_NONE) damage_window(w);
                else if (rect_intersect(r, content, &c)) damage(c);
            }
            break;
        case UI_CLOSE:
            close_window(idx);
            break;
        default:
            break;
        }
    }
    return busy;
}

/* ---- main -------------------------------------------------------------- */

int main(void) {
    sh_gpu_info_t gpu;
    uint64_t q, last_tick = 0;

    mem_set(&fb, 0, sizeof(fb));
    fbmem = (uint8_t *)(uintptr_t)sh_map_framebuffer(&fb);
    if ((long)(uintptr_t)fbmem == -1 || !fbmem || (fb.bpp != 32 && fb.bpp != 24)) {
        print("wm: no usable framebuffer\n");
        return 1;
    }
    if (sh_gpu_query(&gpu) == 0) needs_present = (int)gpu.needs_present;

    screen.px = backbuf;
    screen.w = fb.width > MAX_W ? MAX_W : fb.width;
    screen.h = fb.height > MAX_H ? MAX_H : fb.height;
    screen.stride = screen.w;
    mouse_x = screen.w / 2;
    mouse_y = screen.h / 2;
    build_wallpaper();
    dock_build();

    q = sh_msg_open(UI_WM_QUEUE);
    if (!q || sh_msg_owner(q) != (long)sh_pid()) {
        /* Someone else holds the WM's name: refuse to run under it. */
        print("wm: /ui/wm is owned by another process\n");
        return 1;
    }
    {
        ui_msg_t stale;
        while (sh_msg_recv(q, &stale, 0) == 0) {}
    }
    update_clock();
    damage(R(0, 0, screen.w, screen.h));
    flush_damage();
    print("wm: desktop ready\n");
    (void)sh_spawn("/apps/explorer.app");

    for (;;) {
        int busy = pump_input();
        busy |= pump_messages(q);
        (void)step_animations(); /* paced by the 1-tick sleep: ~100 fps */
        if (sh_ticks() - last_tick >= 25) {
            last_tick = sh_ticks();
            reap_dead_windows();
            update_clock();
        }
        flush_damage();
        if (!busy) sh_sleep(1);
    }
}
