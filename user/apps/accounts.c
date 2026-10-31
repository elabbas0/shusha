/* Accounts: who can sign in to this machine.
 *
 * Everyone can change their own password.  Administrators can also add
 * accounts, reset a password and delete accounts.  Every change asks for
 * your own password again; authd checks it (with the same lockout as the
 * login screen) - this app never sees a hash.
 */
#include "lib.h"
#include "ui.h"
#include "svc.h"
#include "authproto.h"

#define WANT_W 820
#define WANT_H 560
#define HEAD_H 70
#define LIST_W 250

enum { F_ME, F_NEW, F_NEW2, F_ANAME, F_APASS, F_RPASS, NF };
enum { A_FIELD = 1, A_PICK, A_CHANGE, A_ADD, A_ADMIN, A_RESET, A_DELETE };

typedef struct {
    char name[24];
    int uid, admin;
} acc_t;

static int win_w = WANT_W, win_h = WANT_H;
static acc_t accts[32];
static int nacc, sel = -1, me_admin, new_admin;
static char me_name[24];
static char fields[NF][64];
static int focus_f = F_ME;
static char msg[128], key_shown[32];
static int msg_bad;
static char reply[1024];
static int mouse_x, mouse_y;
static uint32_t prev_buttons;

typedef struct {
    rect_t r;
    int action, arg;
} hit_t;
static hit_t hits[48];
static int nhits;

static int auth(const char *verb, const char *data) {
    int st = 1;
    if (svc_request(AUTH_QUEUE, AUTH_IO_DIR, AUTH_BOX, verb, data, data ? str_len(data) : 0, reply, sizeof(reply),
                    &st) < 0) return 1;
    return st;
}

static void set_msg(const char *s, int bad) {
    str_copy(msg, s, sizeof(msg));
    msg_bad = bad;
}

static void msg_from_reply(void) {
    if (str_starts(reply, "fail\tlocked\t")) {
        str_copy(msg, "Too many wrong passwords - wait ", sizeof(msg));
        str_cat(msg, reply + 12, sizeof(msg));
    } else if (str_starts(reply, "fail\twrong")) {
        str_copy(msg, "Your password is not right.", sizeof(msg));
    } else if (str_starts(reply, "fail\t")) {
        str_copy(msg, reply + 5, sizeof(msg));
    } else {
        str_copy(msg, reply, sizeof(msg));
    }
    for (int i = 0; msg[i]; i++) {
        if (msg[i] == '\n' || msg[i] == '\t') msg[i] = msg[i] == '\t' ? ' ' : '\0';
    }
    msg_bad = 1;
}

static void load(void) {
    char *p;
    nacc = 0;
    if (auth("whoami", 0) == 0) {
        int k = 0;
        while (reply[k] && reply[k] != '\t' && k < 23) {
            me_name[k] = reply[k];
            k++;
        }
        me_name[k] = '\0';
        me_admin = 0;
        for (p = reply; *p; p++) {
            if (str_starts(p, "\tadmin")) me_admin = 1;
        }
    }
    if (auth("list", 0) != 0) return;
    for (p = reply; *p && nacc < 32;) {
        acc_t *a = &accts[nacc++];
        int k = 0, uid = 0;
        while (*p && *p != '\t' && k < 23) a->name[k++] = *p++;
        a->name[k] = '\0';
        if (*p == '\t') p++;
        while (*p >= '0' && *p <= '9') uid = uid * 10 + (*p++ - '0');
        a->uid = uid;
        a->admin = str_starts(p, "\tadmin");
        while (*p && *p != '\n') p++;
        if (*p) p++;
    }
    if (sel >= nacc) sel = -1;
}

static void line(char *data, const char *s) {
    str_cat(data, s, 400);
    str_cat(data, "\n", 400);
}

static void wipe(void) {
    for (int i = 0; i < NF; i++) {
        if (i != F_ANAME) mem_wipe(fields[i], sizeof(fields[i]));
    }
}

static void do_change(void) {
    char data[400] = "";
    if (!str_eq(fields[F_NEW], fields[F_NEW2])) {
        set_msg("The new passwords do not match.", 1);
        return;
    }
    line(data, fields[F_ME]);
    line(data, fields[F_NEW]);
    if (auth("passwd", data) == 0) set_msg("Your password was changed.", 0);
    else msg_from_reply();
    mem_wipe(data, sizeof(data));
    wipe();
}

static void do_add(void) {
    char data[400] = "";
    line(data, fields[F_ME]);
    line(data, fields[F_ANAME]);
    line(data, fields[F_APASS]);
    line(data, new_admin ? "1" : "0");
    key_shown[0] = '\0';
    if (auth("create", data) == 0) {
        char *p = reply + 3;
        while (*p && *p != '\t') p++;
        if (*p) p++;
        str_copy(key_shown, p, sizeof(key_shown));
        for (int i = 0; key_shown[i]; i++) {
            if (key_shown[i] == '\n') key_shown[i] = '\0';
        }
        str_copy(msg, "Added ", sizeof(msg));
        str_cat(msg, fields[F_ANAME], sizeof(msg));
        str_cat(msg, ". Give them their recovery key below.", sizeof(msg));
        msg_bad = 0;
        fields[F_ANAME][0] = '\0';
        new_admin = 0;
    } else {
        msg_from_reply();
    }
    mem_wipe(data, sizeof(data));
    wipe();
    load();
}

static void do_reset(void) {
    char data[400] = "";
    if (sel < 0) return;
    line(data, fields[F_ME]);
    line(data, accts[sel].name);
    line(data, fields[F_RPASS]);
    if (auth("reset", data) == 0) set_msg("Password reset.", 0);
    else msg_from_reply();
    mem_wipe(data, sizeof(data));
    wipe();
}

static void do_delete(void) {
    char data[400] = "";
    if (sel < 0) return;
    line(data, fields[F_ME]);
    line(data, accts[sel].name);
    if (auth("delete", data) == 0) {
        set_msg("Account deleted; its files are kept, locked to the system.", 0);
        sel = -1;
    } else {
        msg_from_reply();
    }
    mem_wipe(data, sizeof(data));
    wipe();
    load();
}

/* ---- drawing ----------------------------------------------------------- */

static void add_hit(int x, int y, int w, int h, int a, int arg) {
    if (nhits < 48) {
        hits[nhits].r = (rect_t){ x, y, w, h };
        hits[nhits].action = a;
        hits[nhits].arg = arg;
        nhits++;
    }
}

static int hov(int x, int y, int w, int h) {
    return mouse_x >= x && mouse_y >= y && mouse_x < x + w && mouse_y < y + h;
}

static void field(surface_t *s, int x, int y, int w, int f, const char *label, int secret) {
    int focused = focus_f == f, len = (int)str_len(fields[f]);
    draw_text(s, x, y, label, COL_MUTED);
    y += 18;
    draw_round_fill(s, x, y, w, 30, 5, focused ? COL_ACCENT : COL_EDGE);
    draw_round_fill(s, x + 1, y + 1, w - 2, 28, 4, COL_BG);
    if (secret) {
        for (int i = 0; i < len && i < (w - 24) / 11; i++) draw_circle(s, x + 14 + i * 11, y + 15, 3, COL_TEXT, 255);
        if (focused) draw_fill(s, x + 10 + len * 11, y + 7, 2, 16, COL_ACCENT_HI);
    } else {
        draw_text_clip(s, x + 10, y + 7, fields[f], COL_TEXT, w - 20);
        if (focused) draw_fill(s, x + 10 + text_width(fields[f]), y + 7, 2, 16, COL_ACCENT_HI);
    }
    add_hit(x, y, w, 30, A_FIELD, f);
}

static void button(surface_t *s, int x, int y, int w, const char *label, int primary, int a, int danger) {
    if (danger) {
        draw_round_fill(s, x, y, w, 28, 6, hov(x, y, w, 28) ? 0xf07a70 : COL_DANGER);
        draw_text_f(s, x + (w - text_width_f(label, FONT_BOLD)) / 2, y + 6, label, 0xffffff, FONT_BOLD);
    } else {
        ui_button(s, x, y, w, 28, label, primary, hov(x, y, w, 28));
    }
    add_hit(x, y, w, 28, a, 0);
}

static void section(surface_t *s, int x, int y, int w, int h, const char *title) {
    draw_round_fill(s, x, y, w, h, 8, COL_EDGE);
    draw_round_fill(s, x + 1, y + 1, w - 2, h - 2, 7, COL_PANEL);
    draw_text_f(s, x + 14, y + 10, title, COL_TEXT, FONT_BOLD);
}

static void render(void) {
    surface_t *s = ui_surface();
    int rx = LIST_W + 20, rw = win_w - rx - 18, y;
    char sub[64];
    nhits = 0;
    draw_fill(s, 0, 0, win_w, win_h, COL_BG);
    draw_vgradient(s, 0, 0, win_w, HEAD_H, 0x1d2433, 0x171e28);
    draw_fill(s, 0, HEAD_H - 1, win_w, 1, COL_EDGE);
    draw_text_f(s, 20, 16, "Accounts", COL_TEXT, FONT_BOLD);
    str_copy(sub, "Signed in as ", sizeof(sub));
    str_cat(sub, me_name, sizeof(sub));
    str_cat(sub, me_admin ? " (administrator)" : "", sizeof(sub));
    draw_text(s, 20, 40, sub, COL_MUTED);
    field(s, win_w - 300, 14, 280, F_ME, "Your password (confirms any change)", 1);

    /* account list */
    draw_fill(s, 0, HEAD_H, LIST_W, win_h - HEAD_H, 0x121822);
    draw_text(s, 16, HEAD_H + 12, "People on this machine", COL_MUTED);
    for (int i = 0; i < nacc; i++) {
        int yy = HEAD_H + 38 + i * 46, on = i == sel;
        char ch[2] = { (char)(accts[i].name[0] - 32), 0 };
        if (on || hov(8, yy, LIST_W - 16, 40)) draw_round_fill(s, 8, yy, LIST_W - 16, 40, 6, on ? COL_SELECT : COL_PANEL);
        draw_circle(s, 30, yy + 20, 13, accts[i].admin ? 0x6a2a82 : 0x2a3342, 255);
        draw_text_f(s, 30 - text_width_f(ch, FONT_BOLD) / 2, yy + 12, ch, 0xffffff, FONT_BOLD);
        draw_text_f(s, 52, yy + 4, accts[i].name, COL_TEXT, FONT_BOLD);
        draw_text(s, 52, yy + 21, accts[i].admin ? "administrator" : "standard", COL_MUTED);
        if (str_eq(accts[i].name, me_name)) draw_text(s, LIST_W - 50, yy + 12, "you", COL_ACCENT_HI);
        if (me_admin) add_hit(8, yy, LIST_W - 16, 40, A_PICK, i);
    }

    y = HEAD_H + 14;
    section(s, rx, y, rw, 112, "Change my password");
    field(s, rx + 14, y + 36, (rw - 160) / 2, F_NEW, "New password", 1);
    field(s, rx + 22 + (rw - 160) / 2, y + 36, (rw - 160) / 2, F_NEW2, "Repeat", 1);
    button(s, rx + rw - 124, y + 54, 110, "Change", 1, A_CHANGE, 0);
    y += 126;
    if (me_admin) {
        section(s, rx, y, rw, 112, "Add an account");
        field(s, rx + 14, y + 36, 160, F_ANAME, "Name (a-z, 0-9)", 0);
        field(s, rx + 186, y + 36, 170, F_APASS, "Their password", 1);
        draw_round_fill(s, rx + 370, y + 60, 18, 18, 4, new_admin ? COL_ACCENT : COL_EDGE);
        if (new_admin) draw_text_f(s, rx + 374, y + 60, "x", 0xffffff, FONT_BOLD);
        draw_text(s, rx + 394, y + 60, "admin", COL_TEXT);
        add_hit(rx + 368, y + 56, 80, 26, A_ADMIN, 0);
        button(s, rx + rw - 124, y + 54, 110, "Add", 1, A_ADD, 0);
        y += 126;
        if (key_shown[0]) {
            draw_round_fill(s, rx, y, rw, 46, 8, 0x3a2f12);
            draw_text(s, rx + 14, y + 6, "Recovery key (shown once):", COL_WARN);
            draw_text_f(s, rx + 14, y + 24, key_shown, 0xffe6a0, FONT_MONO);
            y += 58;
        }
        if (sel >= 0) {
            char t[64];
            str_copy(t, "Manage ", sizeof(t));
            str_cat(t, accts[sel].name, sizeof(t));
            section(s, rx, y, rw, 104, t);
            field(s, rx + 14, y + 34, 200, F_RPASS, "New password for them", 1);
            button(s, rx + 226, y + 52, 150, "Reset password", 0, A_RESET, 0);
            button(s, rx + rw - 124, y + 52, 110, "Delete", 0, A_DELETE, 1);
        } else {
            draw_text(s, rx + 4, y + 6, "Pick someone on the left to reset their password or delete them.", COL_MUTED);
        }
    } else {
        draw_text(s, rx + 4, y + 4, "Only administrators can add or remove accounts.", COL_MUTED);
    }
    if (msg[0]) {
        draw_fill(s, LIST_W, win_h - 30, win_w - LIST_W, 30, msg_bad ? 0x3a1c1c : 0x16302c);
        draw_text_clip(s, LIST_W + 14, win_h - 23, msg, msg_bad ? 0xffb4ac : 0x9ef0d8, win_w - LIST_W - 28);
    }
    ui_present();
}

static void perform(int a, int arg) {
    switch (a) {
    case A_FIELD: focus_f = arg; break;
    case A_PICK: sel = arg; focus_f = F_RPASS; break;
    case A_CHANGE: do_change(); break;
    case A_ADD: do_add(); break;
    case A_ADMIN: new_admin = !new_admin; break;
    case A_RESET: do_reset(); break;
    case A_DELETE: do_delete(); break;
    }
}

static int esc;

static void key(uint32_t k) {
    char *f = fields[focus_f];
    int len = (int)str_len(f);
    if (esc) {
        if ((k >= 'A' && k <= 'Z') || k == '~' || k == 'u' || ++esc > 8) esc = 0;
        return;
    }
    if (k == 27) {
        esc = 1;
    } else if (k == '\t') {
        focus_f = (focus_f + 1) % NF;
        if (!me_admin && focus_f > F_NEW2) focus_f = F_ME;
    } else if (k == '\b' || k == 127) {
        if (len) f[len - 1] = '\0';
    } else if (k == '\n' || k == '\r') {
        if (focus_f == F_NEW || focus_f == F_NEW2) do_change();
        else if (focus_f == F_ANAME || focus_f == F_APASS) do_add();
        else if (focus_f == F_RPASS) do_reset();
    } else if (k >= 32 && k <= 126 && len < 63) {
        f[len] = (char)k;
        f[len + 1] = '\0';
    }
}

int main(void) {
    ui_event_t ev;
    load();
    if (ui_open("Accounts", WANT_W, WANT_H) != 0) return 1;
    win_w = ui_surface()->w;
    win_h = ui_surface()->h;
    render();
    for (;;) {
        ui_wait(&ev);
        do {
            if (ev.type == UI_EV_CLOSE) {
                for (int i = 0; i < NF; i++) mem_wipe(fields[i], sizeof(fields[i]));
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
