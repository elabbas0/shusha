#include "ui.h"
#include "ui_proto.h"
#include "lib.h"

/* Private back buffer, sized for the largest window the WM hands out. */
#define UI_MAX_PIXELS (1600 * 1000)

static uint32_t back[UI_MAX_PIXELS];
static surface_t surf;
static uint32_t *shared;
static uint64_t shm_handle;
static uint64_t wm_q, my_q;
static long wm_pid;
static uint32_t win_id;

static void msg_clear(ui_msg_t *m) {
    mem_set(m, 0, sizeof(*m));
}

/* Next message from the WM itself; anything another process managed to
 * send to our mailbox is dropped. */
static int recv_from_wm(ui_msg_t *m) {
    long from;
    while ((from = sh_msg_recv_from(my_q, m)) > 0) {
        if (from == wm_pid) return 0;
    }
    return -1;
}

int ui_open(const char *title, int w, int h) {
    char qname[48];
    ui_msg_t m;
    int tries;

    if ((uint64_t)w * (uint64_t)h > UI_MAX_PIXELS || w <= 0 || h <= 0) return -1;
    wm_q = sh_msg_open(UI_WM_QUEUE);
    if (!wm_q || (wm_pid = sh_msg_owner(wm_q)) <= 0) return -1;
    ui_mailbox_name(qname, sizeof(qname), sh_pid());
    my_q = sh_msg_open(qname);
    if (!my_q) return -1;

    msg_clear(&m);
    m.type = UI_OPEN;
    m.open.w = w;
    m.open.h = h;
    m.open.pid = (uint32_t)sh_pid();
    str_copy(m.open.title, title, UI_TITLE_MAX);
    if (sh_msg_send(wm_q, &m) != 0) return -1;

    /* The WM answers within a frame or two; give up after ~5 s. */
    for (tries = 0; tries < 500; tries++) {
        if (recv_from_wm(&m) == 0) {
            if (m.type == UI_OPEN_OK) break;
            if (m.type == UI_OPEN_FAIL) return -1;
            continue;
        }
        sh_sleep(1);
    }
    if (tries == 500) return -1;

    shared = sh_shm_map(m.open_ok.shm);
    if (!shared) return -1;
    shm_handle = m.open_ok.shm;
    win_id = m.window;
    surf.px = back;
    surf.w = m.open_ok.w;
    surf.h = m.open_ok.h;
    surf.stride = surf.w;
    return 0;
}

surface_t *ui_surface(void) {
    return &surf;
}

void ui_present_rect(int x, int y, int w, int h) {
    rect_t r = { x, y, w, h }, all = { 0, 0, surf.w, surf.h }, c;
    ui_msg_t m;

    if (!shared || !rect_intersect(r, all, &c)) return;
    for (int j = 0; j < c.h; j++) {
        const uint32_t *s = back + (c.y + j) * surf.w + c.x;
        uint32_t *d = shared + (c.y + j) * surf.w + c.x;
        for (int i = 0; i < c.w; i++) d[i] = s[i];
    }
    msg_clear(&m);
    m.type = UI_DAMAGE;
    m.window = win_id;
    m.damage.x = c.x;
    m.damage.y = c.y;
    m.damage.w = c.w;
    m.damage.h = c.h;
    (void)sh_msg_send(wm_q, &m);
}

void ui_present(void) {
    ui_present_rect(0, 0, surf.w, surf.h);
}

int ui_poll(ui_event_t *ev) {
    ui_msg_t m;

    while (recv_from_wm(&m) == 0) {
        if (m.window != win_id) continue;
        mem_set(ev, 0, sizeof(*ev));
        switch (m.type) {
        case UI_KEY:
            ev->type = UI_EV_KEY;
            ev->key = m.key.code;
            return 1;
        case UI_MOUSE:
            ev->type = UI_EV_MOUSE;
            ev->x = m.mouse.x;
            ev->y = m.mouse.y;
            ev->buttons = m.mouse.buttons;
            return 1;
        case UI_CLOSE:
            ev->type = UI_EV_CLOSE;
            return 1;
        case UI_FOCUS:
            ev->type = UI_EV_FOCUS;
            ev->focused = (int)m.focus.focused;
            return 1;
        default:
            break;
        }
    }
    return 0;
}

void ui_wait(ui_event_t *ev) {
    while (!ui_poll(ev)) sh_sleep(1);
}

void ui_close(void) {
    ui_msg_t m;
    if (!shared) return;
    msg_clear(&m);
    m.type = UI_CLOSE;
    m.window = win_id;
    (void)sh_msg_send(wm_q, &m);
    sh_shm_unmap(shm_handle);
    sh_shm_close(shm_handle);
    shared = 0;
}
