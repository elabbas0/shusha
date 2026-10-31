/* greeter: Shusha's login and lock screen.
 *
 *   greeter login          sign in (or, on first boot, create the
 *                          administrator account)
 *   greeter lock <name>    unlock <name>'s session
 *
 * Runs as GREETER_UID: no home, no files, no hashes.  It only passes what
 * is typed to authd, which checks it and - if right - tells the window
 * manager to start or unlock the session.  The window manager shows
 * nothing but this window until then, and only it receives keystrokes.
 */
#include "lib.h"
#include "ui.h"
#include "svc.h"
#include "authproto.h"

#define WANT_W 460
#define WANT_H 500
#define MAXU   12

enum { SC_SETUP, SC_KEY, SC_LOGIN, SC_RECOVER };
enum { F_NAME, F_PASS, F_PASS2, F_KEY, NF };

static int win_w = WANT_W, win_h = WANT_H;
static int screen_kind, lock_mode, busy;
static char fields[NF][64];
static int focus_f = F_PASS;
static char users[MAXU][24];
static int nusers, sel;
static char error[96], info[96], shown_key[32];
static char reply[512];
static int mouse_x, mouse_y;
static uint32_t prev_buttons;

typedef struct {
    rect_t r;
    int action, arg;
} hit_t;
static hit_t hits[32];
static int nhits;

enum { A_FIELD = 1, A_USER, A_SUBMIT, A_FORGOT, A_BACK, A_CONTINUE };

/* ---- authd ------------------------------------------------------------- */

static long auth(const char *verb, const char *data, int *status) {
    long n = svc_request(AUTH_QUEUE, AUTH_IO_DIR, AUTH_BOX, verb, data, data ? str_len(data) : 0, reply,
                         sizeof(reply), status);
    return n;
}

static void load_users(void) {
    char *p;
    nusers = 0;
    if (auth("list", 0, 0) < 0) return;
    for (p = reply; *p && nusers < MAXU;) {
        int k = 0;
        while (*p && *p != '\t' && *p != '\n' && k < 23) users[nusers][k++] = *p++;
        users[nusers][k] = '\0';
        nusers++;
        while (*p && *p != '\n') p++;
        if (*p) p++;
    }
}

/* "fail\t<why>[\t<seconds>]" -> a sentence */
static void set_error_from_reply(void) {
    char *p = reply;
    while (*p && *p != '\t') p++;
    if (*p) p++;
    if (str_starts(p, "locked")) {
        char *s = p + 6;
        while (*s == '\t') s++;
        str_copy(error, "Too many attempts. Try again in ", sizeof(error));
        {
            char n[8];
            int k = 0;
            while (s[k] >= '0' && s[k] <= '9' && k < 7) {
                n[k] = s[k];
                k++;
            }
            n[k] = '\0';
            str_cat(error, n, sizeof(error));
        }
        str_cat(error, " s.", sizeof(error));
    } else if (str_starts(p, "wrong")) {
        str_copy(error, "That password is not right.", sizeof(error));
    } else {
        int k = 0;
        while (p[k] && p[k] != '\n' && k < (int)sizeof(error) - 1) {
            error[k] = p[k];
            k++;
        }
        error[k] = '\0';
    }
}

static void build(char *out, uint64_t cap, const char *a, const char *b, const char *c) {
    str_copy(out, a, cap);
    str_cat(out, "\n", cap);
    str_cat(out, b, cap);
    if (c) {
        str_cat(out, "\n", cap);
        str_cat(out, c, cap);
    }
    str_cat(out, "\n", cap);
}

static void wipe_fields(void) {
    for (int i = F_PASS; i < NF; i++) mem_wipe(fields[i], sizeof(fields[i]));
}

static void render(void);

static void submit(void) {
    char data[256];
    int status;
    error[0] = '\0';
    if (screen_kind == SC_SETUP) {
        if (!fields[F_NAME][0] || !fields[F_PASS][0]) {
            str_copy(error, "Choose a name and a password.", sizeof(error));
            return;
        }
        if (!str_eq(fields[F_PASS], fields[F_PASS2])) {
            str_copy(error, "The passwords do not match.", sizeof(error));
            return;
        }
        busy = 1;
        render();
        build(data, sizeof(data), fields[F_NAME], fields[F_PASS], 0);
        auth("setup", data, &status);
        mem_wipe(data, sizeof(data));
        busy = 0;
        if (status || !str_starts(reply, "ok\t")) {
            set_error_from_reply();
            return;
        }
        str_copy(shown_key, reply + 3, sizeof(shown_key));
        for (int i = 0; shown_key[i]; i++) {
            if (shown_key[i] == '\n') shown_key[i] = '\0';
        }
        screen_kind = SC_KEY; /* sign in once they have noted the key */
        return;
    }
    if (screen_kind == SC_RECOVER) {
        if (!str_eq(fields[F_PASS], fields[F_PASS2])) {
            str_copy(error, "The new passwords do not match.", sizeof(error));
            return;
        }
        busy = 1;
        render();
        build(data, sizeof(data), fields[F_NAME], fields[F_KEY], fields[F_PASS]);
        auth("recover", data, &status);
        mem_wipe(data, sizeof(data));
        busy = 0;
        if (status || !str_starts(reply, "ok\t")) {
            set_error_from_reply();
            return;
        }
        str_copy(shown_key, reply + 3, sizeof(shown_key));
        for (int i = 0; shown_key[i]; i++) {
            if (shown_key[i] == '\n') shown_key[i] = '\0';
        }
        str_copy(info, "Password changed. Your old recovery key no longer works.", sizeof(info));
        screen_kind = SC_KEY;
        return;
    }
    /* sign in / unlock */
    if (!nusers) return;
    busy = 1;
    render();
    build(data, sizeof(data), users[sel], fields[F_PASS], 0);
    auth("login", data, &status);
    mem_wipe(data, sizeof(data));
    busy = 0;
    if (status || !str_starts(reply, "ok\t")) {
        set_error_from_reply();
        mem_wipe(fields[F_PASS], sizeof(fields[F_PASS]));
        return;
    }
    /* authd has told the window manager; our work is done. */
    wipe_fields();
    ui_close();
    sh_exit(0);
}

static void after_key(void) {
    /* From first-boot setup or a recovery: go to sign in. */
    str_copy(fields[F_NAME], fields[F_NAME], sizeof(fields[F_NAME]));
    load_users();
    for (int i = 0; i < nusers; i++) {
        if (str_eq(users[i], fields[F_NAME])) sel = i;
    }
    mem_wipe(shown_key, sizeof(shown_key));
    wipe_fields();
    screen_kind = SC_LOGIN;
    focus_f = F_PASS;
}

/* ---- drawing ----------------------------------------------------------- */

static void add_hit(int x, int y, int w, int h, int action, int arg) {
    if (nhits < 32) {
        hits[nhits].r = (rect_t){ x, y, w, h };
        hits[nhits].action = action;
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
    y += 20;
    draw_round_fill(s, x, y, w, 34, 6, focused ? COL_ACCENT : COL_EDGE);
    draw_round_fill(s, x + 1, y + 1, w - 2, 32, 5, COL_BG);
    if (secret) {
        for (int i = 0; i < len && i < (w - 30) / 12; i++) draw_circle(s, x + 16 + i * 12, y + 17, 4, COL_TEXT, 255);
        if (focused) draw_fill(s, x + 12 + len * 12, y + 9, 2, 16, COL_ACCENT_HI);
    } else {
        draw_text_clip_f(s, x + 12, y + 9, fields[f], COL_TEXT, w - 24, f == F_KEY ? FONT_MONO : FONT_SANS);
        if (focused) {
            int cx = x + 12 + text_width_f(fields[f], f == F_KEY ? FONT_MONO : FONT_SANS);
            draw_fill(s, cx < x + w - 6 ? cx : x + w - 6, y + 9, 2, 16, COL_ACCENT_HI);
        }
    }
    add_hit(x, y, w, 34, A_FIELD, f);
}

static void button(surface_t *s, int x, int y, int w, const char *label, int primary, int action) {
    ui_button(s, x, y, w, 34, label, primary, hov(x, y, w, 34));
    add_hit(x, y, w, 34, action, 0);
}

static void avatar(surface_t *s, int cx, int cy, int r, const char *name, int on) {
    char ch[2] = { name[0] >= 'a' && name[0] <= 'z' ? (char)(name[0] - 32) : name[0], 0 };
    draw_circle(s, cx, cy, r + 2, on ? COL_ACCENT_HI : COL_EDGE, 255);
    draw_circle(s, cx, cy, r, on ? 0x6a2a82 : 0x2a3342, 255);
    draw_text_f(s, cx - text_width_f(ch, FONT_BOLD) / 2, cy - 8, ch, 0xffffff, FONT_BOLD);
}

static void center(surface_t *s, int y, const char *t, uint32_t c, int font) {
    draw_text_f(s, (win_w - text_width_f(t, font)) / 2, y, t, c, font);
}

static void render(void) {
    surface_t *s = ui_surface();
    int x = 40, w = win_w - 80, y;
    nhits = 0;
    draw_vgradient(s, 0, 0, win_w, win_h, 0x1c1228, 0x10151d);
    draw_icon(s, (win_w - ICON_SIZE) / 2, 22, ICON_WORKSPACE);
    y = 84;

    if (screen_kind == SC_SETUP) {
        center(s, y, "Welcome to Shusha", COL_TEXT, FONT_BOLD);
        center(s, y + 22, "Create the administrator account for this machine.", COL_MUTED, FONT_SANS);
        field(s, x, y + 58, w, F_NAME, "Name (a-z, 0-9)", 0);
        field(s, x, y + 120, w, F_PASS, "Password (at least 8 characters)", 1);
        field(s, x, y + 182, w, F_PASS2, "Repeat password", 1);
        button(s, x, y + 254, w, busy ? "Creating..." : "Create account", 1, A_SUBMIT);
        center(s, y + 302, "Stored only as an Argon2id hash with a machine key.", 0x5b6676, FONT_SANS);
    } else if (screen_kind == SC_KEY) {
        center(s, y, "Your recovery key", COL_TEXT, FONT_BOLD);
        center(s, y + 24, "Write it down and keep it somewhere safe. It is shown", COL_MUTED, FONT_SANS);
        center(s, y + 42, "only once and is the only way back in without your password.", COL_MUTED, FONT_SANS);
        draw_round_fill(s, x, y + 82, w, 54, 8, COL_EDGE);
        draw_round_fill(s, x + 1, y + 83, w - 2, 52, 7, COL_BG);
        center(s, y + 100, shown_key, COL_ACCENT_HI, FONT_MONO);
        if (info[0]) center(s, y + 154, info, COL_WARN, FONT_SANS);
        button(s, x, y + 200, w, "I have written it down", 1, A_CONTINUE);
    } else if (screen_kind == SC_LOGIN) {
        if (lock_mode) {
            avatar(s, win_w / 2, y + 40, 34, users[sel], 1);
            center(s, y + 88, users[sel], COL_TEXT, FONT_BOLD);
            center(s, y + 110, "This session is locked.", COL_MUTED, FONT_SANS);
        } else {
            int n = nusers > 5 ? 5 : nusers, step = 78, x0 = (win_w - n * step) / 2;
            center(s, y, "Sign in", COL_TEXT, FONT_BOLD);
            for (int i = 0; i < n; i++) {
                int cx = x0 + i * step + step / 2;
                avatar(s, cx, y + 60, 26, users[i], i == sel);
                draw_text_clip(s, cx - text_width(users[i]) / 2, y + 94, users[i], i == sel ? COL_TEXT : COL_MUTED, step);
                add_hit(cx - 34, y + 30, 68, 84, A_USER, i);
            }
        }
        field(s, x, y + 140, w, F_PASS, "Password", 1);
        button(s, x, y + 214, w, busy ? "Checking..." : (lock_mode ? "Unlock" : "Sign in"), 1, A_SUBMIT);
        if (!lock_mode) {
            const char *f = "Forgot your password?";
            int fw = text_width(f);
            draw_text(s, (win_w - fw) / 2, y + 262, f, hov((win_w - fw) / 2, y + 260, fw, 20) ? COL_ACCENT_HI : COL_MUTED);
            add_hit((win_w - fw) / 2, y + 260, fw, 20, A_FORGOT, 0);
        }
    } else {
        center(s, y, "Recover your account", COL_TEXT, FONT_BOLD);
        field(s, x, y + 34, w, F_NAME, "Name", 0);
        field(s, x, y + 94, w, F_KEY, "Recovery key", 0);
        field(s, x, y + 154, w, F_PASS, "New password", 1);
        field(s, x, y + 214, w, F_PASS2, "Repeat new password", 1);
        button(s, x, y + 282, w / 2 - 6, "Back", 0, A_BACK);
        button(s, x + w / 2 + 6, y + 282, w / 2 - 6, busy ? "Checking..." : "Set password", 1, A_SUBMIT);
    }
    if (error[0]) center(s, win_h - 34, error, COL_DANGER, FONT_SANS);
    ui_present();
}

/* ---- input ------------------------------------------------------------- */

static void perform(int a, int arg) {
    error[0] = '\0';
    switch (a) {
    case A_FIELD: focus_f = arg; break;
    case A_USER: sel = arg; focus_f = F_PASS; mem_wipe(fields[F_PASS], sizeof(fields[F_PASS])); break;
    case A_SUBMIT: submit(); break;
    case A_FORGOT:
        screen_kind = SC_RECOVER;
        str_copy(fields[F_NAME], nusers ? users[sel] : "", sizeof(fields[F_NAME]));
        focus_f = F_KEY;
        break;
    case A_BACK: screen_kind = SC_LOGIN; wipe_fields(); focus_f = F_PASS; break;
    case A_CONTINUE: after_key(); break;
    }
}

static void next_field(void) {
    static const int setup_order[] = { F_NAME, F_PASS, F_PASS2 };
    static const int recover_order[] = { F_NAME, F_KEY, F_PASS, F_PASS2 };
    const int *o = screen_kind == SC_SETUP ? setup_order : recover_order;
    int n = screen_kind == SC_SETUP ? 3 : 4;
    if (screen_kind == SC_LOGIN) return;
    for (int i = 0; i < n; i++) {
        if (o[i] == focus_f) {
            focus_f = o[(i + 1) % n];
            return;
        }
    }
    focus_f = o[0];
}

static int esc;

static void key(uint32_t k) {
    char *f = fields[focus_f];
    int len = (int)str_len(f);
    if (esc) { /* swallow escape sequences (arrows etc.) */
        if ((k >= 'A' && k <= 'Z') || k == '~' || (k >= 'a' && k <= 'z' && esc > 1)) esc = 0;
        else esc++;
        if (esc > 8) esc = 0;
        return;
    }
    if (k == 27) {
        esc = 1;
        return;
    }
    if (screen_kind == SC_KEY) {
        if (k == '\n' || k == '\r') after_key();
        return;
    }
    if (k == '\n' || k == '\r') submit();
    else if (k == '\t') next_field();
    else if (k == '\b' || k == 127) {
        if (len) f[len - 1] = '\0';
    } else if (k >= 32 && k <= 126 && len < 63) {
        f[len] = (char)k;
        f[len + 1] = '\0';
    }
    error[0] = '\0';
}

int main(int argc, char **argv) {
    ui_event_t ev;
    lock_mode = argc > 2 && str_eq(argv[1], "lock");
    load_users();
    if (lock_mode) {
        nusers = 1;
        str_copy(users[0], argv[2], sizeof(users[0]));
        sel = 0;
        screen_kind = SC_LOGIN;
    } else {
        screen_kind = nusers ? SC_LOGIN : SC_SETUP;
        focus_f = nusers ? F_PASS : F_NAME;
    }
    if (ui_open(lock_mode ? "Locked" : "Sign in", WANT_W, WANT_H) != 0) return 1;
    win_w = ui_surface()->w;
    win_h = ui_surface()->h;
    render();
    for (;;) {
        int redraw = 0;
        ui_wait(&ev);
        do {
            if (ev.type == UI_EV_KEY) {
                key(ev.key);
                redraw = 1;
            } else if (ev.type == UI_EV_MOUSE) {
                uint32_t pressed = ev.buttons & ~prev_buttons;
                prev_buttons = ev.buttons;
                mouse_x = ev.x;
                mouse_y = ev.y;
                redraw = 1;
                if (pressed & UI_BTN_LEFT) {
                    for (int i = nhits - 1; i >= 0; i--) {
                        if (rect_contains(hits[i].r, ev.x, ev.y)) {
                            perform(hits[i].action, hits[i].arg);
                            break;
                        }
                    }
                }
            }
            /* UI_EV_CLOSE is ignored: the login screen stays. */
        } while (ui_poll(&ev));
        if (redraw) render();
    }
}
