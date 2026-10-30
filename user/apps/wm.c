/* Shusha window manager / compositor.
 *
 * Design goals: never repaint or upload more than changed.
 *  - Every change becomes a damage rectangle (window moved, client
 *    flushed a region, cursor moved, clock ticked).
 *  - Each frame composites only the damaged rectangles into a RAM back
 *    buffer, through a clipped view so drawing outside is free, then
 *    copies exactly those rectangles to the write-combined framebuffer.
 *  - Clients draw privately and push only their damaged rect (ui.c),
 *    so window content is copied once per change, not per frame.
 *  - With no input, messages or damage the loop sleeps a tick.
 */
#include "lib.h"
#include "draw.h"
#include "ui_proto.h"

#define MAX_W     1920
#define MAX_H     1200
#define MAX_WIN   16
#define MAX_DMG   32
#define PANEL_H   30
#define TITLE_H   26
#define CLOSE_W   26

typedef struct {
    int used;
    uint32_t id;
    uint32_t pid;
    uint64_t q;
    uint64_t shm;
    uint32_t *px;
    int x, y, w, h; /* content rectangle, screen coordinates */
    char title[UI_TITLE_MAX];
} win_t;

static uint32_t backbuf[MAX_W * MAX_H];
static surface_t screen;
static sh_fb_info_t fb;
static uint8_t *fbmem;
static int needs_present;

static win_t wins[MAX_WIN];
static int zorder[MAX_WIN]; /* bottom .. top, indices into wins */
static int zcount;
static int focus = -1;
static uint32_t next_id = 1;

static rect_t dmg[MAX_DMG];
static int ndmg;

static int mouse_x, mouse_y;
static uint32_t mouse_buttons;
static int drag = -1, drag_dx, drag_dy;
static int capture = -1; /* window receiving mouse while a button is held */
static int panel_hover = -1;

static uint32_t wallpaper_row[MAX_H];
static char clock_text[16];

/* ---- damage ------------------------------------------------------------ */

static void damage(rect_t r) {
    rect_t all = { 0, 0, screen.w, screen.h }, c;
    if (!rect_intersect(r, all, &c)) return;
    /* Merge into an overlapping rect so the list stays short. */
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

static rect_t frame_rect(const win_t *w) {
    rect_t r = { w->x - 1, w->y - TITLE_H, w->w + 2, w->h + TITLE_H + 1 };
    return r;
}

static rect_t close_rect(const win_t *w) {
    rect_t r = { w->x + w->w - CLOSE_W, w->y - TITLE_H, CLOSE_W, TITLE_H };
    return r;
}

/* ---- panel ------------------------------------------------------------- */

enum { PB_EXPLORER, PB_TERMINAL, PB_POWER, PB_COUNT };
static const char *const panel_labels[PB_COUNT] = { "Explorer", "Terminal", "Power off" };

static rect_t panel_button(int i) {
    rect_t r = { 0, 4, 0, PANEL_H - 8 };
    if (i == PB_POWER) {
        r.w = 96;
        r.x = screen.w - r.w - 8;
    } else {
        r.w = 92;
        r.x = 96 + i * (r.w + 8);
    }
    return r;
}

static rect_t clock_rect(void) {
    rect_t r = { screen.w - 96 - 8 - 150, 0, 140, PANEL_H };
    return r;
}

static void update_clock(void) {
    char t[16], n[8];
    uint64_t s = sh_ticks() / 100;
    t[0] = '\0';
    str_cat(t, "up ", sizeof(t));
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

static const char *const cursor_img[] = {
    "X           ", "XX          ", "X.X         ", "X..X        ", "X...X       ",
    "X....X      ", "X.....X     ", "X......X    ", "X.......X   ", "X........X  ",
    "X.........X ", "X......XXXXX", "X...X..X    ", "X..XX..X    ", "X.X  X..X   ",
    "XX   X..X   ", "X     X..X  ", "      X..X  ", "       XX   ",
};
#define CURSOR_W 12
#define CURSOR_H 19

static rect_t cursor_rect(void) {
    rect_t r = { mouse_x, mouse_y, CURSOR_W, CURSOR_H };
    return r;
}

static void paint_window(surface_t *v, int ox, int oy, const win_t *w, int focused) {
    rect_t f = frame_rect(w), c = close_rect(w);
    uint32_t bar = focused ? COL_PANEL_HI : COL_PANEL;

    draw_fill(v, f.x + ox + 4, f.y + oy + 4, f.w, f.h, 0x070a0f); /* shadow */
    draw_frame(v, f.x + ox, f.y + oy, f.w, f.h, focused ? COL_ACCENT : COL_EDGE);
    draw_fill(v, w->x + ox, w->y - TITLE_H + 1 + oy, w->w, TITLE_H - 1, bar);
    draw_text_clip(v, w->x + 10 + ox, w->y - TITLE_H + 5 + oy, w->title,
                   focused ? COL_TEXT : COL_MUTED, w->w - CLOSE_W - 20);
    draw_text(v, c.x + 9 + ox, c.y + 5 + oy, "x", focused ? COL_DANGER : COL_MUTED);
    draw_blit(v, w->x + ox, w->y + oy, w->px, w->w, w->w, w->h);
}

static void paint_panel(surface_t *v, int ox, int oy) {
    draw_fill(v, ox, oy, screen.w, PANEL_H, COL_PANEL);
    draw_fill(v, ox, PANEL_H - 1 + oy, screen.w, 1, COL_EDGE);
    draw_text(v, 14 + ox, 7 + oy, "SHUSHA", COL_ACCENT_HI);
    for (int i = 0; i < PB_COUNT; i++) {
        rect_t b = panel_button(i);
        ui_button(v, b.x + ox, b.y + oy, b.w, b.h, panel_labels[i], 0, i == panel_hover);
    }
    {
        rect_t c = clock_rect();
        draw_text(v, c.x + c.w - text_width(clock_text) + ox, 7 + oy, clock_text, COL_MUTED);
    }
}

static void composite(rect_t r) {
    /* A view onto the back buffer clipped to r; everything is drawn in
     * screen coordinates shifted by (ox, oy), so clipping is implicit. */
    surface_t v = { backbuf + r.y * screen.stride + r.x, r.w, r.h, screen.stride };
    int ox = -r.x, oy = -r.y;

    for (int j = 0; j < r.h; j++) {
        uint32_t c = wallpaper_row[r.y + j];
        uint32_t *row = v.px + j * v.stride;
        for (int i = 0; i < r.w; i++) row[i] = c;
    }
    draw_text(&v, screen.w - 220 + ox, screen.h - 40 + oy, "shusha - secure workspace", 0x1d3a44);

    for (int k = 0; k < zcount; k++) {
        const win_t *w = &wins[zorder[k]];
        rect_t f = frame_rect(w), shadow = { f.x, f.y, f.w + 4, f.h + 4 }, tmp;
        if (rect_intersect(shadow, r, &tmp)) paint_window(&v, ox, oy, w, zorder[k] == focus);
    }
    if (r.y < PANEL_H) paint_panel(&v, ox, oy);

    {
        rect_t cr = cursor_rect(), tmp;
        if (rect_intersect(cr, r, &tmp)) {
            for (int j = 0; j < CURSOR_H; j++) {
                for (int i = 0; i < CURSOR_W; i++) {
                    char p = cursor_img[j][i];
                    if (p == 'X') draw_fill(&v, mouse_x + i + ox, mouse_y + j + oy, 1, 1, 0x000000);
                    else if (p == '.') draw_fill(&v, mouse_x + i + ox, mouse_y + j + oy, 1, 1, 0xffffff);
                }
            }
        }
    }
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
        damage(frame_rect(&wins[idx]));
    }
}

static int win_by_id(uint32_t id) {
    for (int i = 0; i < MAX_WIN; i++) {
        if (wins[i].used && wins[i].id == id) return i;
    }
    return -1;
}

static void remove_window(int idx) {
    win_t *w = &wins[idx];
    rect_t f = frame_rect(w);
    int k, j = 0;

    f.w += 4;
    f.h += 4;
    damage(f);
    sh_shm_unmap(w->shm);
    sh_shm_close(w->shm);
    w->used = 0;
    for (k = 0; k < zcount; k++) {
        if (zorder[k] != idx) zorder[j++] = zorder[k];
    }
    zcount = j;
    if (drag == idx) drag = -1;
    if (capture == idx) capture = -1;
    if (focus == idx) {
        focus = -1;
        if (zcount) set_focus(zorder[zcount - 1]);
    }
}

static void open_window(const ui_msg_t *req) {
    char qname[32] = "/ui/p";
    ui_msg_t m;
    int idx = -1, w = req->open.w, h = req->open.h;
    uint64_t q;
    static int cascade;

    fmt_u64(qname + 5, req->open.pid, 10);
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
    if (h > screen.h - PANEL_H - TITLE_H - 20) h = screen.h - PANEL_H - TITLE_H - 20;
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
    win->pid = req->open.pid;
    win->q = q;
    win->w = w;
    win->h = h;
    win->x = (screen.w - w) / 2 + (cascade % 6) * 28 - 70;
    win->y = PANEL_H + TITLE_H + 24 + (cascade % 6) * 28;
    cascade++;
    if (win->x < 8) win->x = 8;
    if (win->x + w > screen.w - 8) win->x = screen.w - 8 - w;
    if (win->y + h > screen.h - 8) win->y = PANEL_H + TITLE_H + 8;
    str_copy(win->title, req->open.title, UI_TITLE_MAX);
    zorder[zcount++] = idx;

    m.type = UI_OPEN_OK;
    m.open_ok.shm = win->shm;
    m.open_ok.w = w;
    m.open_ok.h = h;
    send(win, &m);
    set_focus(idx);
    damage(frame_rect(win));
}

static void move_window(int idx, int x, int y) {
    win_t *w = &wins[idx];
    rect_t before = frame_rect(w);
    before.w += 4;
    before.h += 4;
    if (y - TITLE_H < PANEL_H) y = PANEL_H + TITLE_H;
    if (y > screen.h - 8) y = screen.h - 8;
    if (x + w->w < 40) x = 40 - w->w;
    if (x > screen.w - 40) x = screen.w - 40;
    if (x == w->x && y == w->y) return;
    w->x = x;
    w->y = y;
    damage(before);
    {
        rect_t after = frame_rect(w);
        after.w += 4;
        after.h += 4;
        damage(after);
    }
}

static void reap_dead_windows(void) {
    for (int i = 0; i < MAX_WIN; i++) {
        sh_proc_info_t info;
        if (!wins[i].used) continue;
        if (sh_proc_info(wins[i].pid, &info) != 0 || info.state == SH_PROC_EXITED) remove_window(i);
    }
}

/* ---- input ------------------------------------------------------------- */

static int window_at(int x, int y) {
    for (int k = zcount - 1; k >= 0; k--) {
        rect_t f = frame_rect(&wins[zorder[k]]);
        if (rect_contains(f, x, y)) return zorder[k];
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

static void panel_click(int i) {
    if (i == PB_EXPLORER) (void)sh_spawn("/apps/explorer.app");
    else if (i == PB_TERMINAL) (void)sh_spawn("/apps/terminal.app");
    else if (i == PB_POWER) {
        (void)sh_sync();
        (void)sh_power(0);
    }
}

static void mouse_event(const sh_mouse_t *e) {
    uint32_t prev = mouse_buttons;
    int moved = e->abs_x != mouse_x || e->abs_y != mouse_y;
    int pressed, released;

    if (moved) damage(cursor_rect());
    mouse_x = e->abs_x;
    mouse_y = e->abs_y;
    if (mouse_x >= screen.w) mouse_x = screen.w - 1;
    if (mouse_y >= screen.h) mouse_y = screen.h - 1;
    if (mouse_x < 0) mouse_x = 0;
    if (mouse_y < 0) mouse_y = 0;
    mouse_buttons = e->buttons & 7;
    if (moved) damage(cursor_rect());
    pressed = (mouse_buttons & ~prev) & 7;
    released = (prev & ~mouse_buttons) & 7;

    {
        int hover = -1;
        if (mouse_y < PANEL_H) {
            for (int i = 0; i < PB_COUNT; i++) {
                if (rect_contains(panel_button(i), mouse_x, mouse_y)) hover = i;
            }
        }
        if (hover != panel_hover) {
            if (panel_hover >= 0) damage(panel_button(panel_hover));
            if (hover >= 0) damage(panel_button(hover));
            panel_hover = hover;
        }
    }

    if (drag >= 0) {
        if (moved) move_window(drag, mouse_x - drag_dx, mouse_y - drag_dy);
        if (released & UI_BTN_LEFT) drag = -1;
        return;
    }

    if (pressed) {
        if (mouse_y < PANEL_H) {
            if ((pressed & UI_BTN_LEFT) && panel_hover >= 0) panel_click(panel_hover);
            return;
        }
        int idx = window_at(mouse_x, mouse_y);
        if (idx < 0) return;
        set_focus(idx);
        win_t *w = &wins[idx];
        if (mouse_y < w->y) {
            if ((pressed & UI_BTN_LEFT) && rect_contains(close_rect(w), mouse_x, mouse_y)) {
                ui_msg_t m;
                mem_set(&m, 0, sizeof(m));
                m.type = UI_CLOSE;
                send(w, &m);
            } else if (pressed & UI_BTN_LEFT) {
                drag = idx;
                drag_dx = mouse_x - w->x;
                drag_dy = mouse_y - w->y;
            }
            return;
        }
        capture = idx;
        send_mouse(idx);
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
        if (rect_contains(content, mouse_x, mouse_y)) send_mouse(focus);
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
        if (focus >= 0) {
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
    int busy = 0;

    while (sh_msg_recv(q, &m, 0) == 0) {
        int idx;
        busy = 1;
        switch (m.type) {
        case UI_OPEN:
            open_window(&m);
            break;
        case UI_DAMAGE:
            idx = win_by_id(m.window);
            if (idx >= 0) {
                win_t *w = &wins[idx];
                rect_t r = { w->x + m.damage.x, w->y + m.damage.y, m.damage.w, m.damage.h };
                rect_t content = { w->x, w->y, w->w, w->h }, c;
                if (rect_intersect(r, content, &c)) damage(c);
            }
            break;
        case UI_CLOSE:
            idx = win_by_id(m.window);
            if (idx >= 0) remove_window(idx);
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
    uint64_t q, last_reap = 0;

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
    for (int y = 0; y < screen.h; y++) {
        /* deep slate fading to teal-black */
        int t = y * 255 / (screen.h > 1 ? screen.h - 1 : 1);
        int r = 0x12 - t * 0x08 / 255, g = 0x1a + t * 0x0c / 255, b = 0x26 + t * 0x02 / 255;
        wallpaper_row[y] = ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
    }

    q = sh_msg_open(UI_WM_QUEUE);
    {
        ui_msg_t stale;
        while (sh_msg_recv(q, &stale, 0) == 0) {}
    }
    update_clock();
    {
        rect_t all = { 0, 0, screen.w, screen.h };
        damage(all);
    }
    flush_damage();
    print("wm: desktop ready\n");
    (void)sh_spawn("/apps/explorer.app");

    for (;;) {
        int busy = pump_input();
        busy |= pump_messages(q);
        if (sh_ticks() - last_reap >= 50) {
            last_reap = sh_ticks();
            reap_dead_windows();
            update_clock();
        }
        flush_damage();
        if (!busy) sh_sleep(1);
    }
}
