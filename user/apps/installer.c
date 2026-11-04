/* Install Shusha / Storage.
 *
 * In a live session (nothing is saved): pick a disk, confirm, enter your
 * own administrator password, and Shusha is installed on it.  The kernel
 * (kernel/fs/store.c, through authd) partitions the disk, writes GRUB and
 * the kernel, makes a machine key bound to this machine's hardware, and
 * saves everything from then on, encrypted.  The recovery code is shown
 * here once.
 *
 * On an installed machine the same window shows where things are saved,
 * how the disk is bound to the hardware, and saves on request.
 */
#include "lib.h"
#include "ui.h"
#include "svc.h"
#include "authproto.h"

#define WANT_W 760
#define WANT_H 520
#define HEAD_H 74

enum { A_DISK = 1, A_ERASE, A_FIELD, A_INSTALL, A_SAVE };

static int win_w = WANT_W, win_h = WANT_H;
static char reply[2048], st[2048];
static char state[16], diskline[64], gen[24], saved[24], hwline[64], ids[48], note[96];
static struct { int index; char text[64]; int in_use; } disks[8];
static int ndisks, sel = -1, erase_ok, can_install, admin, busy;
static char pw[64], code[40], msg[128];
static int msg_bad;
static int mouse_x, mouse_y;
static uint32_t prev_buttons;

typedef struct {
    rect_t r;
    int action, arg;
} hit_t;
static hit_t hits[32];
static int nhits;

static int auth(const char *verb, const char *data) {
    int s = 1;
    if (svc_request(AUTH_QUEUE, AUTH_IO_DIR, AUTH_BOX, verb, data, data ? str_len(data) : 0, reply, sizeof(reply),
                    &s) < 0) return 1;
    return s;
}

static void take(const char *src, char *out, uint64_t cap) {
    uint64_t n = 0;
    while (src[n] && src[n] != '\n' && n + 1 < cap) {
        out[n] = src[n];
        n++;
    }
    out[n] = 0;
}

static void load(void) {
    char *p;
    state[0] = diskline[0] = gen[0] = saved[0] = hwline[0] = ids[0] = note[0] = 0;
    ndisks = 0;
    can_install = 0;
    admin = 0;
    if (auth("whoami", 0) == 0) {
        for (p = reply; *p; p++) {
            if (str_starts(p, "\tadmin")) admin = 1;
        }
    }
    if (auth("store", 0) != 0) return;
    str_copy(st, reply, sizeof(st));
    for (p = st; *p;) {
        if (str_starts(p, "state ")) take(p + 6, state, sizeof(state));
        else if (str_starts(p, "disk ")) take(p + 5, diskline, sizeof(diskline));
        else if (str_starts(p, "generation ")) take(p + 11, gen, sizeof(gen));
        else if (str_starts(p, "saved ")) take(p + 6, saved, sizeof(saved));
        else if (str_starts(p, "hardware ")) take(p + 9, hwline, sizeof(hwline));
        else if (str_starts(p, "ids ")) take(p + 4, ids, sizeof(ids));
        else if (str_starts(p, "note ")) take(p + 5, note, sizeof(note));
        else if (str_starts(p, "can-install yes")) can_install = 1;
        else if (str_starts(p, "device ") && ndisks < 8) {
            int i = 0;
            char *q = p + 7;
            while (*q >= '0' && *q <= '9') i = i * 10 + (*q++ - '0');
            disks[ndisks].index = i;
            while (*q == ' ') q++;
            take(q, disks[ndisks].text, sizeof(disks[ndisks].text));
            disks[ndisks].in_use = 0;
            for (char *r = disks[ndisks].text; *r; r++) {
                if (str_starts(r, " in-use")) {
                    *r = 0;
                    disks[ndisks].in_use = 1;
                    break;
                }
            }
            ndisks++;
        }
        while (*p && *p != '\n') p++;
        if (*p) p++;
    }
    if (sel < 0) {
        for (int i = 0; i < ndisks; i++) {
            if (!disks[i].in_use) { sel = i; break; }
        }
    }
}

static int live(void) {
    return str_eq(state, "none");
}

static void set_msg(const char *s, int bad) {
    str_copy(msg, s, sizeof(msg));
    msg_bad = bad;
}

static void do_install(void) {
    char data[128], n[8];
    if (sel < 0 || sel >= ndisks) return set_msg("Pick a disk first.", 1);
    if (!erase_ok) return set_msg("Tick the box to confirm the disk may be erased.", 1);
    if (!pw[0]) return set_msg("Enter your password to confirm.", 1);
    str_copy(data, pw, sizeof(data));
    str_cat(data, "\n", sizeof(data));
    str_cat(data, fmt_u64(n, (uint64_t)disks[sel].index, 10), sizeof(data));
    str_cat(data, "\n", sizeof(data));
    mem_wipe(pw, sizeof(pw));
    if (auth("install", data) == 0 && str_starts(reply, "ok\t")) {
        take(reply + 3, code, sizeof(code));
        set_msg("Shusha is installed. Remove the install media and restart (menu at the top right).", 0);
    } else {
        if (str_starts(reply, "fail\twrong")) set_msg("Your password is not right.", 1);
        else if (str_starts(reply, "fail\t")) {
            char t[100];
            take(reply + 5, t, sizeof(t));
            for (int i = 0; t[i]; i++) if (t[i] == '\t') t[i] = ' ';
            set_msg(t, 1);
        } else set_msg("The installer did not answer.", 1);
    }
    mem_wipe(data, sizeof(data));
    load();
}

/* ---- drawing ------------------------------------------------------------------ */

static void add_hit(int x, int y, int w, int h, int a, int arg) {
    if (nhits < 32) {
        hits[nhits].r = (rect_t){ x, y, w, h };
        hits[nhits].action = a;
        hits[nhits].arg = arg;
        nhits++;
    }
}

static int hov(int x, int y, int w, int h) {
    return mouse_x >= x && mouse_y >= y && mouse_x < x + w && mouse_y < y + h;
}

static void card(surface_t *s, int x, int y, int w, int h, const char *title) {
    draw_round_fill(s, x, y, w, h, 8, COL_EDGE);
    draw_round_fill(s, x + 1, y + 1, w - 2, h - 2, 7, COL_PANEL);
    if (title) draw_text_f(s, x + 16, y + 12, title, COL_TEXT, FONT_BOLD);
}

static void row(surface_t *s, int x, int y, const char *k, const char *v) {
    draw_text(s, x, y, k, COL_MUTED);
    draw_text_f(s, x + 150, y, v, COL_TEXT, FONT_BOLD);
}

static void render_installed(surface_t *s) {
    int x = 20, y = HEAD_H + 16, w = win_w - 40;
    char t[96];
    card(s, x, y, w, 200, "Saved on this machine");
    str_copy(t, diskline, sizeof(t));
    str_cat(t, ", encrypted", sizeof(t));
    row(s, x + 16, y + 44, "Disk", t);
    str_copy(t, "save ", sizeof(t));
    str_cat(t, gen, sizeof(t));
    str_cat(t, ", ", sizeof(t));
    str_cat(t, saved, sizeof(t));
    row(s, x + 16, y + 70, "Last save", t);
    row(s, x + 16, y + 96, "Hardware binding", hwline);
    row(s, x + 16, y + 122, "Bound to", ids);
    draw_text_clip(s, x + 16, y + 154,
                   "Any two of the system UUID, network card and disk open it. If two change, the recovery code does.",
                   COL_MUTED, w - 32);
    ui_button(s, x + w - 140, y + 12, 124, 28, "Save now", 1, hov(x + w - 140, y + 12, 124, 28));
    add_hit(x + w - 140, y + 12, 124, 28, A_SAVE, 0);
    y += 214;
    card(s, x, y, w, 96, "What is kept");
    draw_text_clip(s, x + 16, y + 40, "Accounts, every home folder, and this machine's workspace, with their owners",
                   COL_TEXT, w - 32);
    draw_text_clip(s, x + 16, y + 60, "and permissions. Changes are saved within a few seconds, and before power off.",
                   COL_TEXT, w - 32);
}

static void render_live(surface_t *s) {
    int x = 20, y = HEAD_H + 14, w = win_w - 40;
    card(s, x, y, w, 70, 0);
    if (code[0]) {
        draw_round_fill(s, x + 14, y + 14, 8, 42, 4, COL_ACCENT);
        draw_text_f(s, x + 34, y + 14, "Shusha is installed. From now on, everything is kept.", COL_TEXT, FONT_BOLD);
        draw_text_clip(s, x + 34, y + 36, "Saved encrypted on the disk, and bound to this machine's hardware.",
                       COL_MUTED, w - 50);
    } else {
        draw_round_fill(s, x + 14, y + 14, 8, 42, 4, COL_WARN);
        draw_text_f(s, x + 34, y + 14, "This is a live session: nothing is kept after a restart.", COL_TEXT, FONT_BOLD);
        draw_text_clip(s, x + 34, y + 36, "Install Shusha on a disk to keep accounts, files and the workspace, encrypted.",
                       COL_MUTED, w - 50);
    }
    y += 84;
    if (code[0]) {
        draw_round_fill(s, x, y, w, 150, 8, 0x3a2f12);
        draw_text_f(s, x + 16, y + 14, "Recovery code - write it down, it is shown only now", COL_WARN, FONT_BOLD);
        draw_text_f(s, x + 16, y + 48, code, 0xffe6a0, FONT_MONO);
        draw_text_clip(s, x + 16, y + 82, "The disk opens only on this machine. If its hardware changes, or the disk",
                       0xf2dca8, w - 32);
        draw_text_clip(s, x + 16, y + 102, "moves to another machine, the login screen asks for this code.", 0xf2dca8,
                       w - 32);
        return;
    }
    if (!can_install) {
        draw_text(s, x + 4, y + 4, "This system cannot install itself (no boot files).", COL_MUTED);
        return;
    }
    card(s, x, y, w, 60 + ndisks * 40, "Install on");
    for (int i = 0; i < ndisks; i++) {
        int yy = y + 42 + i * 40, on = i == sel;
        char t[96];
        if (on || hov(x + 10, yy, w - 20, 34)) draw_round_fill(s, x + 10, yy, w - 20, 34, 6, on ? COL_SELECT : COL_BG);
        draw_circle(s, x + 30, yy + 17, 7, on ? COL_ACCENT : COL_EDGE, 255);
        if (on) draw_circle(s, x + 30, yy + 17, 3, 0xffffff, 255);
        str_copy(t, "Disk ", sizeof(t));
        {
            char n[8];
            str_cat(t, fmt_u64(n, (uint64_t)disks[i].index, 10), sizeof(t));
        }
        str_cat(t, "  -  ", sizeof(t));
        str_cat(t, disks[i].text, sizeof(t));
        draw_text_f(s, x + 46, yy + 9, t, COL_TEXT, FONT_BOLD);
        if (disks[i].in_use) draw_text(s, x + w - 120, yy + 9, "in use", COL_MUTED);
        else add_hit(x + 10, yy, w - 20, 34, A_DISK, i);
    }
    y += 70 + ndisks * 40;
    draw_round_fill(s, x + 4, y + 4, 18, 18, 4, erase_ok ? COL_DANGER : COL_EDGE);
    if (erase_ok) draw_text_f(s, x + 8, y + 4, "x", 0xffffff, FONT_BOLD);
    draw_text(s, x + 30, y + 5, "Erase everything on this disk and install Shusha", COL_TEXT);
    add_hit(x, y, 420, 26, A_ERASE, 0);
    y += 40;
    if (!admin) {
        draw_text(s, x + 4, y, "Only an administrator can install.", COL_MUTED);
        return;
    }
    draw_text(s, x + 4, y, "Your password (confirms the install)", COL_MUTED);
    draw_round_fill(s, x, y + 20, 300, 30, 5, COL_ACCENT);
    draw_round_fill(s, x + 1, y + 21, 298, 28, 4, COL_BG);
    for (int i = 0; pw[i] && i < 24; i++) draw_circle(s, x + 14 + i * 11, y + 35, 3, COL_TEXT, 255);
    draw_fill(s, x + 10 + (int)str_len(pw) * 11, y + 27, 2, 16, COL_ACCENT_HI);
    add_hit(x, y + 20, 300, 30, A_FIELD, 0);
    if (busy) {
        draw_text_f(s, x + 320, y + 26, "Installing... the screen may pause.", COL_WARN, FONT_BOLD);
    } else {
        int bx = x + 320, by = y + 20;
        draw_round_fill(s, bx, by, 190, 30, 6, hov(bx, by, 190, 30) ? 0xf07a70 : COL_DANGER);
        draw_text_f(s, bx + (190 - text_width_f("Erase and install", FONT_BOLD)) / 2, by + 7, "Erase and install",
                    0xffffff, FONT_BOLD);
        add_hit(bx, by, 190, 30, A_INSTALL, 0);
    }
}

static void render(void) {
    surface_t *s = ui_surface();
    nhits = 0;
    draw_fill(s, 0, 0, win_w, win_h, COL_BG);
    draw_vgradient(s, 0, 0, win_w, HEAD_H, 0x1d2433, 0x171e28);
    draw_fill(s, 0, HEAD_H - 1, win_w, 1, COL_EDGE);
    if (live() || code[0]) {
        /* still "Install Shusha" right after installing, to show the code */
        draw_text_f(s, 20, 16, "Install Shusha", COL_TEXT, FONT_BOLD);
        draw_text(s, 20, 42, "Keep accounts, files and the workspace on this machine", COL_MUTED);
        render_live(s);
    } else {
        draw_text_f(s, 20, 16, "Storage", COL_TEXT, FONT_BOLD);
        draw_text(s, 20, 42, str_eq(state, "unlocked") ? "Shusha is installed on this machine"
                                                          : "The saved state is not open", COL_MUTED);
        render_installed(s);
    }
    if (msg[0]) {
        draw_fill(s, 0, win_h - 30, win_w, 30, msg_bad ? 0x3a1c1c : 0x16302c);
        draw_text_clip(s, 14, win_h - 23, msg, msg_bad ? 0xffb4ac : 0x9ef0d8, win_w - 28);
    }
    ui_present();
}

static void perform(int a, int arg) {
    switch (a) {
    case A_DISK: sel = arg; break;
    case A_ERASE: erase_ok = !erase_ok; break;
    case A_SAVE:
        set_msg(sh_sync() == 0 ? "Saved." : "Could not save.", 0);
        load();
        break;
    case A_INSTALL:
        busy = 1;
        render();
        do_install();
        busy = 0;
        break;
    }
}

static int esc;

static void key(uint32_t k) {
    int len = (int)str_len(pw);
    if (esc) {
        if ((k >= 'A' && k <= 'Z') || k == '~' || k == 'u' || ++esc > 8) esc = 0;
        return;
    }
    if (k == 27) esc = 1;
    else if (k == '\b' || k == 127) { if (len) pw[len - 1] = 0; }
    else if (k == '\n' || k == '\r') perform(A_INSTALL, 0);
    else if (k >= 32 && k <= 126 && len < 63) { pw[len] = (char)k; pw[len + 1] = 0; }
}

int main(void) {
    ui_event_t ev;
    load();
    if (ui_open(live() ? "Install Shusha" : "Storage", WANT_W, WANT_H) != 0) return 1;
    win_w = ui_surface()->w;
    win_h = ui_surface()->h;
    render();
    for (;;) {
        ui_wait(&ev);
        do {
            if (ev.type == UI_EV_CLOSE) {
                mem_wipe(pw, sizeof(pw));
                mem_wipe(code, sizeof(code));
                ui_close();
                return 0;
            }
            if (ev.type == UI_EV_KEY) {
                key(ev.key);
            } else if (ev.type == UI_EV_MOUSE) {
                uint32_t pressed = ev.buttons & ~prev_buttons;
                prev_buttons = ev.buttons;
                mouse_x = ev.x;
                mouse_y = ev.y;
                if (pressed & UI_BTN_LEFT) {
                    for (int i = nhits - 1; i >= 0; i--) {
                        if (rect_contains(hits[i].r, ev.x, ev.y)) {
                            perform(hits[i].action, hits[i].arg);
                            break;
                        }
                    }
                }
            }
        } while (ui_poll(&ev));
        render();
    }
}
