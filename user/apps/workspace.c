/* Shusha Workspace: the desktop front end for workspaced.
 *
 * Create or join a workspace, see who is in it (ranks, who is online),
 * share files, open the ones your rank allows, ask for and grant access,
 * and follow what happens - refreshed every few seconds from the
 * service.  Everything goes through wsclient.c, so the app can only add
 * files this user can read and only save copies where this user can
 * write: personal files stay personal unless added here.
 */
#include "lib.h"
#include "ui.h"
#include "wsclient.h"

#define WANT_W 880
#define WANT_H 580
#define HEAD_H 72
#define SIDE_W 176
#define FOOT_H 30
#define ROW_H  44
#define REFRESH_TICKS 250

enum { V_OVERVIEW, V_MEMBERS, V_FILES, V_REQUESTS, V_ACTIVITY };
enum {
    A_NONE, A_NAV, A_CREATE, A_JOIN, A_INVITE, A_SYNC, A_ADD, A_ADD_OK, A_CANCEL, A_OPEN, A_SAVE,
    A_REQUEST, A_GRANT, A_DENY, A_FOCUS, A_BACK
};
enum { F_CREATE, F_CODE, F_NAME, F_PATH, NFIELDS };

typedef struct {
    int rank, state; /* state: 0 offline, 1 online, 2 you */
    char name[24], ip[16], key[16];
} mem_t;

typedef struct {
    char id[33], name[65], owner[24];
    int owner_rank, open, size;
} file_t;

typedef struct {
    int rank;
    char name[24], id[33], file[65];
} req_t;

typedef struct {
    int ago;
    char text[80];
} evt_t;

typedef struct {
    rect_t r;
    int action, arg;
} hit_t;

static int win_w = WANT_W, win_h = WANT_H;
static int view = V_OVERVIEW;

/* What the service told us at the last refresh. */
static int active, foreign, my_rank, n_members, n_online, n_requests;
static char ws_name[32], address[16], device[16];
static mem_t members[16];
static int nmem;
static file_t files[48];
static int nfiles;
static req_t reqs[16];
static int nreqs;
static evt_t evts[12];
static int nevts;

static char reply_buf[WS_MAX_FILE + 1024];
static char message[128] = "";
static char invite[80];
static uint64_t last_refresh;

/* File viewer */
static int viewing = -1;
static char view_buf[WS_MAX_FILE + 1];
static int view_top;

/* Text fields and the add-file dialog */
static char fields[NFIELDS][96] = { "", "", "", "/home/" };
static int focus_field = F_CREATE, dialog;

static hit_t hits[96];
static int nhits;
static int mouse_x, mouse_y, hover_hit = -1;
static uint32_t prev_buttons;

/* ---- talking to the service ------------------------------------------- */

static void set_message(const char *s) {
    int i = 0;
    while (s[i] && s[i] != '\n' && i < (int)sizeof(message) - 1) {
        message[i] = s[i];
        i++;
    }
    message[i] = '\0';
}

static long ask(const char *line) {
    return ws_request(line, 0, 0, reply_buf, sizeof(reply_buf), 0);
}

/* Copy the next tab/newline-terminated field of *p into dst. */
static void field(char **p, char *dst, uint64_t cap) {
    uint64_t n = 0;
    while (**p && **p != '\t' && **p != '\n') {
        if (n + 1 < cap) dst[n++] = **p;
        (*p)++;
    }
    dst[n] = '\0';
    if (**p == '\t') (*p)++;
}

static int num(const char *s) {
    int v = 0;
    while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0');
    return v;
}

static void end_line(char **p) {
    while (**p && **p != '\n') (*p)++;
    if (**p) (*p)++;
}

static void refresh(void) {
    char *p, k[24], v[64];
    last_refresh = sh_ticks();
    active = foreign = 0;
    nmem = nfiles = nreqs = nevts = 0;
    if (ask("q status") < 0) {
        set_message(reply_buf);
        return;
    }
    for (p = reply_buf; *p;) {
        field(&p, k, sizeof(k));
        field(&p, v, sizeof(v));
        end_line(&p);
        if (str_eq(k, "active")) active = num(v);
        else if (str_eq(k, "foreign")) foreign = num(v);
        else if (str_eq(k, "name")) str_copy(ws_name, v, sizeof(ws_name));
        else if (str_eq(k, "rank")) my_rank = num(v);
        else if (str_eq(k, "members")) n_members = num(v);
        else if (str_eq(k, "online")) n_online = num(v);
        else if (str_eq(k, "requests")) n_requests = num(v);
        else if (str_eq(k, "address")) str_copy(address, v, sizeof(address));
        else if (str_eq(k, "device")) str_copy(device, v, sizeof(device));
    }
    if (!active) return;

    if (ask("q members") >= 0) {
        for (p = reply_buf; *p && nmem < 16;) {
            mem_t *m = &members[nmem++];
            field(&p, v, sizeof(v));
            m->rank = num(v);
            field(&p, m->name, sizeof(m->name));
            field(&p, m->ip, sizeof(m->ip));
            field(&p, v, sizeof(v));
            m->state = str_eq(v, "you") ? 2 : str_eq(v, "online");
            field(&p, m->key, sizeof(m->key));
            end_line(&p);
        }
    }
    if (ask("q files") >= 0) {
        for (p = reply_buf; *p && nfiles < 48;) {
            file_t *f = &files[nfiles++];
            field(&p, f->id, sizeof(f->id));
            field(&p, f->name, sizeof(f->name));
            field(&p, v, sizeof(v));
            f->owner_rank = num(v);
            field(&p, f->owner, sizeof(f->owner));
            field(&p, v, sizeof(v));
            f->open = str_eq(v, "open");
            field(&p, v, sizeof(v));
            f->size = num(v);
            end_line(&p);
        }
    }
    if (ask("q requests") >= 0) {
        for (p = reply_buf; *p && nreqs < 16;) {
            req_t *r = &reqs[nreqs++];
            field(&p, v, sizeof(v));
            r->rank = num(v);
            field(&p, r->name, sizeof(r->name));
            field(&p, r->id, sizeof(r->id));
            field(&p, r->file, sizeof(r->file));
            end_line(&p);
        }
    }
    if (ask("q events") >= 0) {
        for (p = reply_buf; *p && nevts < 12;) {
            evt_t *e = &evts[nevts++];
            field(&p, v, sizeof(v));
            e->ago = num(v);
            field(&p, e->text, sizeof(e->text));
            end_line(&p);
        }
    }
    if (viewing >= nfiles) viewing = -1;
}

/* Run a user command (wsclient handles put/get on our side) and show the
 * first line of the answer. */
static void run(const char *line) {
    long n = ws_run(line, reply_buf, sizeof(reply_buf));
    set_message(n < 0 || reply_buf[0] ? reply_buf : "Done");
    refresh();
}

/* ---- actions ----------------------------------------------------------- */

static void do_create(void) {
    char line[128];
    if (!fields[F_CREATE][0]) {
        set_message("Give the workspace a name");
        return;
    }
    str_copy(line, "create ", sizeof(line));
    str_cat(line, fields[F_CREATE], sizeof(line));
    run(line);
}

static void do_join(void) {
    char line[160];
    if (!fields[F_CODE][0]) {
        set_message("Paste or type the invite code");
        return;
    }
    set_message("Joining - verifying the workspace...");
    str_copy(line, "join ", sizeof(line));
    str_cat(line, fields[F_CODE], sizeof(line));
    str_cat(line, " ", sizeof(line));
    str_cat(line, fields[F_NAME][0] ? fields[F_NAME] : "member", sizeof(line));
    run(line);
}

static void do_invite(void) {
    char *p;
    if (ask("invite") < 0) {
        set_message(reply_buf);
        return;
    }
    p = reply_buf;
    while (*p && !str_starts(p, "shu")) end_line(&p);
    if (!*p) {
        set_message(reply_buf);
        return;
    }
    {
        int i = 0;
        while (p[i] && p[i] != '\n' && i < (int)sizeof(invite) - 1) {
            invite[i] = p[i];
            i++;
        }
        invite[i] = '\0';
    }
    /* Also to the system console (serial on test builds), like Terminal. */
    print("workspace: invite ");
    print(invite);
    print("\n");
    set_message("Invite created - single use, share it with the person joining");
}

static void do_open(int i) {
    char line[48];
    int failed;
    long n;
    str_copy(line, "raw #", sizeof(line));
    str_cat(line, files[i].id, sizeof(line));
    n = ws_request(line, 0, 0, view_buf, sizeof(view_buf), &failed);
    if (n < 0 || failed) {
        set_message(view_buf);
        return;
    }
    viewing = i;
    view_top = 0;
    set_message("Decrypted on this machine - not saved anywhere unless you save a copy");
}

static void do_save(int i) {
    char line[160];
    str_copy(line, "get #", sizeof(line));
    str_cat(line, files[i].id, sizeof(line));
    str_cat(line, " /home/", sizeof(line));
    str_cat(line, files[i].name, sizeof(line));
    run(line);
}

static void do_file_cmd(const char *verb, const char *id, int rank) {
    char line[64], n[8];
    str_copy(line, verb, sizeof(line));
    str_cat(line, " #", sizeof(line));
    str_cat(line, id, sizeof(line));
    if (rank >= 0) {
        str_cat(line, " ", sizeof(line));
        str_cat(line, fmt_u64(n, (uint64_t)rank, 10), sizeof(line));
    }
    run(line);
}

static void do_add(void) {
    char line[128];
    str_copy(line, "put ", sizeof(line));
    str_cat(line, fields[F_PATH], sizeof(line));
    dialog = 0;
    run(line);
}

static void perform(int action, int arg) {
    switch (action) {
    case A_NAV: view = arg; viewing = -1; break;
    case A_CREATE: do_create(); break;
    case A_JOIN: do_join(); break;
    case A_INVITE: do_invite(); break;
    case A_SYNC: set_message("Syncing..."); run("sync"); break;
    case A_ADD: dialog = 1; focus_field = F_PATH; break;
    case A_ADD_OK: do_add(); break;
    case A_CANCEL: dialog = 0; break;
    case A_OPEN: do_open(arg); break;
    case A_SAVE: do_save(arg); break;
    case A_REQUEST: do_file_cmd("request", files[arg].id, -1); break;
    case A_GRANT: do_file_cmd("grant", reqs[arg].id, reqs[arg].rank); break;
    case A_DENY: do_file_cmd("deny", reqs[arg].id, reqs[arg].rank); break;
    case A_FOCUS: focus_field = arg; break;
    case A_BACK: viewing = -1; break;
    }
}

/* ---- drawing helpers --------------------------------------------------- */

static void add_hit(int x, int y, int w, int h, int action, int arg) {
    if (nhits < (int)(sizeof(hits) / sizeof(hits[0]))) {
        hits[nhits].r = (rect_t){ x, y, w, h };
        hits[nhits].action = action;
        hits[nhits].arg = arg;
        nhits++;
    }
}

static int hovered(int x, int y, int w, int h) {
    return mouse_x >= x && mouse_y >= y && mouse_x < x + w && mouse_y < y + h;
}

static void button(surface_t *s, int x, int y, int w, const char *label, int primary, int action, int arg) {
    ui_button(s, x, y, w, 28, label, primary, hovered(x, y, w, 28));
    add_hit(x, y, w, 28, action, arg);
}

static void card(surface_t *s, int x, int y, int w, int h) {
    draw_round_fill(s, x, y, w, h, 8, COL_EDGE);
    draw_round_fill(s, x + 1, y + 1, w - 2, h - 2, 7, COL_PANEL);
}

static void input(surface_t *s, int x, int y, int w, int f, const char *placeholder) {
    int focused = focus_field == f;
    draw_round_fill(s, x, y, w, 30, 5, focused ? COL_ACCENT : COL_EDGE);
    draw_round_fill(s, x + 1, y + 1, w - 2, 28, 4, COL_BG);
    if (fields[f][0]) draw_text_clip_f(s, x + 10, y + 7, fields[f], COL_TEXT, w - 20, f == F_CODE ? FONT_MONO : FONT_SANS);
    else draw_text_clip(s, x + 10, y + 7, placeholder, 0x5b6676, w - 20);
    if (focused) {
        int cx = x + 10 + text_width_f(fields[f], f == F_CODE ? FONT_MONO : FONT_SANS);
        if (cx > x + w - 8) cx = x + w - 8;
        draw_fill(s, cx, y + 7, 2, 16, COL_ACCENT_HI);
    }
    add_hit(x, y, w, 30, A_FOCUS, f);
}

static void rank_badge(surface_t *s, int x, int y, int rank) {
    char t[16], n[8];
    str_copy(t, rank == 0 ? "admin" : "rank ", sizeof(t));
    if (rank) str_cat(t, fmt_u64(n, (uint64_t)rank, 10), sizeof(t));
    draw_rrect(s, x, y, text_width_f(t, FONT_BOLD) + 14, 20, 10, RR_ALL, rank == 0 ? 0x7a2a8c : 0x24465a, 255);
    draw_text_f(s, x + 7, y + 2, t, 0xffffff, FONT_BOLD);
}

static void size_text(char *out, int size) {
    char n[16];
    str_copy(out, fmt_u64(n, (uint64_t)size, 10), 24);
    str_cat(out, size == 1 ? " byte" : " bytes", 24);
}

/* ---- views ------------------------------------------------------------- */

static void draw_none(surface_t *s, int x, int y, int w) {
    int cw = (w - 16) / 2, ch = 220;
    draw_text_f(s, x, y, "Start working together", COL_TEXT, FONT_BOLD);
    draw_text(s, x, y + 22, "A workspace is a separate, encrypted space shared by Shusha machines.", COL_MUTED);
    y += 56;
    card(s, x, y, cw, ch);
    draw_text_f(s, x + 18, y + 16, "Create a workspace", COL_TEXT, FONT_BOLD);
    draw_text_clip(s, x + 18, y + 40, "You become its admin (rank 0).", COL_MUTED, cw - 36);
    input(s, x + 18, y + 76, cw - 36, F_CREATE, "Workspace name");
    button(s, x + 18, y + 120, 110, "Create", 1, A_CREATE, 0);

    card(s, x + cw + 16, y, cw, ch);
    draw_text_f(s, x + cw + 34, y + 16, "Join with an invite", COL_TEXT, FONT_BOLD);
    draw_text_clip(s, x + cw + 34, y + 40, "Ranks follow join order.", COL_MUTED, cw - 36);
    input(s, x + cw + 34, y + 76, cw - 36, F_CODE, "Invite code (shu...)");
    input(s, x + cw + 34, y + 116, cw - 36, F_NAME, "Your name");
    button(s, x + cw + 34, y + 160, 110, "Join", 1, A_JOIN, 0);

    y += ch + 18;
    card(s, x, y, w, 64);
    draw_icon(s, x + 16, y + 24, ICON_LOCK);
    draw_text_clip(s, x + 44, y + 13, "Your personal files stay on this machine and stay yours.", COL_TEXT, w - 60);
    draw_text_clip(s, x + 44, y + 33, "Nothing enters the workspace unless you add it.", COL_MUTED, w - 60);
}

static void stat_card(surface_t *s, int x, int y, int w, const char *label, const char *value, const char *sub,
                      uint32_t accent) {
    card(s, x, y, w, 86);
    draw_fill(s, x + 1, y + 12, 3, 62, accent);
    draw_text_clip(s, x + 16, y + 12, label, COL_MUTED, w - 24);
    draw_text_f(s, x + 16, y + 36, value, COL_TEXT, FONT_BOLD);
    if (sub) draw_text_clip(s, x + 16, y + 58, sub, COL_MUTED, w - 24);
}

static void draw_overview(surface_t *s, int x, int y, int w) {
    char a[32], b[32], n[16];
    int cw = (w - 36) / 4, locked = 0;
    for (int i = 0; i < nfiles; i++) locked += !files[i].open;

    str_copy(a, fmt_u64(n, (uint64_t)my_rank, 10), sizeof(a));
    stat_card(s, x, y, cw, "Your rank", a, my_rank == 0 ? "admin" : "can open ranks below", 0xb04ad0);
    str_copy(a, fmt_u64(n, (uint64_t)n_online, 10), sizeof(a));
    str_cat(a, " of ", sizeof(a));
    str_cat(a, fmt_u64(n, (uint64_t)n_members, 10), sizeof(a));
    stat_card(s, x + cw + 12, y, cw, "Online", a, "members", 0x4ade80);
    str_copy(a, fmt_u64(n, (uint64_t)nfiles, 10), sizeof(a));
    str_copy(b, fmt_u64(n, (uint64_t)locked, 10), sizeof(b));
    str_cat(b, " locked for you", sizeof(b));
    stat_card(s, x + 2 * (cw + 12), y, cw, "Shared files", a, b, COL_ACCENT);
    str_copy(a, fmt_u64(n, (uint64_t)n_requests, 10), sizeof(a));
    stat_card(s, x + 3 * (cw + 12), y, cw, "Requests", a, n_requests ? "waiting for you" : "none waiting",
              n_requests ? COL_DANGER : 0x5b6676);
    y += 104;

    if (invite[0]) {
        card(s, x, y, w, 78);
        draw_text_f(s, x + 16, y + 12, "Invite code", COL_TEXT, FONT_BOLD);
        draw_text_clip(s, x + 120, y + 12, "single use - the next person to join gets the next rank", COL_MUTED, w - 140);
        draw_round_fill(s, x + 16, y + 38, w - 32, 28, 5, COL_BG);
        draw_text_clip_f(s, x + 26, y + 44, invite, COL_ACCENT_HI, w - 52, FONT_MONO);
        y += 92;
    }

    draw_text_f(s, x, y, "Recent activity", COL_TEXT, FONT_BOLD);
    y += 26;
    if (!nevts) draw_text(s, x, y, "Nothing yet.", COL_MUTED);
    for (int i = 0; i < nevts && i < (invite[0] ? 4 : 6); i++) {
        char ago[24];
        draw_circle(s, x + 6, y + 9, 3, i == 0 ? COL_ACCENT_HI : 0x5b6676, 255);
        draw_text_clip(s, x + 18, y, evts[i].text, i == 0 ? COL_TEXT : 0xc8d1dc, w - 120);
        str_copy(ago, fmt_u64(n, (uint64_t)evts[i].ago, 10), sizeof(ago));
        str_cat(ago, "s ago", sizeof(ago));
        draw_text(s, x + w - text_width(ago), y, ago, COL_MUTED);
        y += 24;
    }
}

static void draw_members(surface_t *s, int x, int y, int w) {
    draw_text_clip(s, x, y, "Higher ranks open lower ranks' files freely; lower ranks need permission.", COL_MUTED, w);
    y += 30;
    for (int i = 0; i < nmem; i++) {
        mem_t *m = &members[i];
        uint32_t dot = m->state == 2 ? COL_ACCENT_HI : m->state ? 0x4ade80 : 0x5b6676;
        card(s, x, y, w, ROW_H);
        draw_circle(s, x + 20, y + ROW_H / 2, 5, dot, 255);
        rank_badge(s, x + 36, y + 12, m->rank);
        draw_text_f(s, x + 120, y + 13, m->name, COL_TEXT, FONT_BOLD);
        if (m->state == 2) draw_text(s, x + 128 + text_width_f(m->name, FONT_BOLD), y + 13, "(you)", COL_MUTED);
        draw_text(s, x + w - 330, y + 13, m->state == 2 ? "this machine" : m->state ? "online" : "offline",
                  m->state == 1 ? 0x4ade80 : COL_MUTED);
        draw_text_f(s, x + w - 226, y + 13, m->ip, 0xc8d1dc, FONT_MONO);
        draw_text_f(s, x + w - 104, y + 13, m->key, 0x7b8798, FONT_MONO);
        y += ROW_H + 8;
    }
}

static void draw_viewer(surface_t *s, int x, int y, int w, int h) {
    file_t *f = &files[viewing];
    char by[64], n[8];
    int line = 0, cy;
    button(s, x, y, 70, "Back", 0, A_BACK, 0);
    draw_text_f(s, x + 84, y + 5, f->name, COL_TEXT, FONT_BOLD);
    str_copy(by, "by ", sizeof(by));
    str_cat(by, f->owner, sizeof(by));
    str_cat(by, f->owner_rank ? " (rank " : " (admin", sizeof(by));
    if (f->owner_rank) str_cat(by, fmt_u64(n, (uint64_t)f->owner_rank, 10), sizeof(by));
    str_cat(by, ")", sizeof(by));
    draw_text(s, x + 96 + text_width_f(f->name, FONT_BOLD), y + 5, by, COL_MUTED);
    button(s, x + w - 170, y, 170, "Save copy to Home", 1, A_SAVE, viewing);
    y += 40;
    card(s, x, y, w, h - 40);
    cy = y + 12;
    for (char *p = view_buf; *p && cy < y + h - 56;) {
        char row[128];
        int k = 0;
        while (*p && *p != '\n' && k < (int)sizeof(row) - 1) row[k++] = *p++;
        row[k] = '\0';
        if (*p == '\n') p++;
        if (line++ < view_top) continue;
        draw_text_clip_f(s, x + 14, cy, row, COL_TEXT, w - 28, FONT_MONO);
        cy += GLYPH_H + 2;
    }
}

static void draw_files(surface_t *s, int x, int y, int w, int h) {
    if (viewing >= 0) {
        draw_viewer(s, x, y, w, h);
        return;
    }
    draw_text_clip(s, x, y + 6, "Encrypted on the owner's machine; every member keeps a copy.", COL_MUTED, w - 140);
    button(s, x + w - 120, y, 120, "Add file...", 1, A_ADD, 0);
    y += 42;
    if (!nfiles) draw_text(s, x, y, "No shared files yet. Add one of yours with \"Add file...\".", COL_MUTED);
    for (int i = 0; i < nfiles && y + ROW_H < win_h - FOOT_H; i++) {
        file_t *f = &files[i];
        char meta[64], n[8], sz[24];
        card(s, x, y, w, ROW_H);
        if (f->open) {
            draw_fill(s, x + 16, y + 12, 14, 18, 0xe2e8f0);
            draw_fill(s, x + 19, y + 17, 8, 2, 0x8c98aa);
            draw_fill(s, x + 19, y + 22, 8, 2, 0x8c98aa);
        } else {
            draw_icon(s, x + 15, y + 14, ICON_LOCK);
        }
        draw_text_clip_f(s, x + 44, y + 6, f->open ? f->name : "Locked file", f->open ? COL_TEXT : 0xc8d1dc,
                         w - 360, FONT_BOLD);
        str_copy(meta, f->owner, sizeof(meta));
        str_cat(meta, f->owner_rank ? "  \xc2\xb7  rank " : "  \xc2\xb7  admin", sizeof(meta));
        if (f->owner_rank) str_cat(meta, fmt_u64(n, (uint64_t)f->owner_rank, 10), sizeof(meta));
        if (f->open) {
            size_text(sz, f->size);
            str_cat(meta, "  \xc2\xb7  ", sizeof(meta));
            str_cat(meta, sz, sizeof(meta));
        } else {
            str_cat(meta, "  \xc2\xb7  name hidden", sizeof(meta));
        }
        draw_text_clip(s, x + 44, y + 23, meta, COL_MUTED, w - 360);
        if (f->open) {
            button(s, x + w - 214, y + 8, 64, "Open", 0, A_OPEN, i);
            button(s, x + w - 142, y + 8, 130, "Save copy", 0, A_SAVE, i);
        } else {
            button(s, x + w - 164, y + 8, 152, "Request access", 1, A_REQUEST, i);
        }
        y += ROW_H + 8;
    }
}

static void draw_requests(surface_t *s, int x, int y, int w) {
    draw_text_clip(s, x, y, "Members below you asking to open your files. Granting adds an encrypted key for them.",
                   COL_MUTED, w);
    y += 30;
    if (!nreqs) draw_text(s, x, y, "No requests waiting.", COL_MUTED);
    for (int i = 0; i < nreqs; i++) {
        req_t *r = &reqs[i];
        char t[128];
        card(s, x, y, w, ROW_H);
        rank_badge(s, x + 14, y + 12, r->rank);
        str_copy(t, r->name, sizeof(t));
        str_cat(t, " asks for ", sizeof(t));
        str_cat(t, r->file[0] ? r->file : "a file", sizeof(t));
        draw_text_clip(s, x + 96, y + 13, t, COL_TEXT, w - 300);
        button(s, x + w - 190, y + 8, 84, "Grant", 1, A_GRANT, i);
        button(s, x + w - 98, y + 8, 84, "Deny", 0, A_DENY, i);
        y += ROW_H + 8;
    }
}

static void draw_activity(surface_t *s, int x, int y, int w) {
    char ago[24], n[16];
    if (!nevts) draw_text(s, x, y, "Nothing yet.", COL_MUTED);
    for (int i = 0; i < nevts; i++) {
        card(s, x, y, w, 34);
        draw_circle(s, x + 16, y + 17, 4, i == 0 ? COL_ACCENT_HI : 0x5b6676, 255);
        draw_text_clip(s, x + 30, y + 8, evts[i].text, COL_TEXT, w - 140);
        str_copy(ago, fmt_u64(n, (uint64_t)evts[i].ago, 10), sizeof(ago));
        str_cat(ago, "s ago", sizeof(ago));
        draw_text(s, x + w - 14 - text_width(ago), y + 8, ago, COL_MUTED);
        y += 40;
    }
}

static void draw_dialog(surface_t *s) {
    int w = 460, h = 170, x = (win_w - w) / 2, y = (win_h - h) / 2;
    draw_fill_a(s, 0, 0, win_w, win_h, 0x000000, 120);
    draw_shadow(s, x, y, w, h, 8, 18, 0x000000, 140);
    draw_round_fill(s, x, y, w, h, 8, COL_ACCENT);
    draw_round_fill(s, x + 1, y + 1, w - 2, h - 2, 7, COL_PANEL);
    draw_text_f(s, x + 18, y + 16, "Add one of your files", COL_TEXT, FONT_BOLD);
    draw_text_clip(s, x + 18, y + 40, "A copy is encrypted and shared; the original stays where it is.", COL_MUTED, w - 36);
    input(s, x + 18, y + 72, w - 36, F_PATH, "/home/file.txt");
    button(s, x + w - 214, y + h - 46, 96, "Share", 1, A_ADD_OK, 0);
    button(s, x + w - 110, y + h - 46, 92, "Cancel", 0, A_CANCEL, 0);
}

static void render(void) {
    surface_t *s = ui_surface();
    static const char *const nav[] = { "Overview", "Members", "Files", "Requests", "Activity" };
    int cx = SIDE_W + 24, cy = HEAD_H + 20, cw = win_w - SIDE_W - 48, ch = win_h - HEAD_H - FOOT_H - 40;
    char sub[96], n[16];

    nhits = 0;
    draw_fill(s, 0, 0, win_w, win_h, COL_BG);
    /* header */
    draw_vgradient(s, 0, 0, win_w, HEAD_H, 0x241433, 0x171e28);
    draw_fill(s, 0, HEAD_H - 1, win_w, 1, COL_EDGE);
    draw_icon(s, 16, 12, ICON_WORKSPACE);
    draw_text_f(s, 76, 16, active ? ws_name : "Workspace", COL_TEXT, FONT_BOLD);
    if (active) {
        str_copy(sub, my_rank == 0 ? "You are the admin" : "You are rank ", sizeof(sub));
        if (my_rank) str_cat(sub, fmt_u64(n, (uint64_t)my_rank, 10), sizeof(sub));
        str_cat(sub, "  \xc2\xb7  ", sizeof(sub));
        str_cat(sub, fmt_u64(n, (uint64_t)n_online, 10), sizeof(sub));
        str_cat(sub, " of ", sizeof(sub));
        str_cat(sub, fmt_u64(n, (uint64_t)n_members, 10), sizeof(sub));
        str_cat(sub, " online  \xc2\xb7  end-to-end encrypted", sizeof(sub));
    } else {
        str_copy(sub, foreign ? "In use by another user of this machine" : "Not in a workspace yet", sizeof(sub));
    }
    draw_text_clip(s, 76, 40, sub, 0xb8a6c9, win_w - 400);
    if (active) {
        if (my_rank == 0) button(s, win_w - 254, 22, 130, "Invite someone", 1, A_INVITE, 0);
        button(s, win_w - 112, 22, 96, "Sync now", 0, A_SYNC, 0);
    }

    /* sidebar */
    draw_fill(s, 0, HEAD_H, SIDE_W, win_h - HEAD_H - FOOT_H, 0x121822);
    draw_fill(s, SIDE_W, HEAD_H, 1, win_h - HEAD_H - FOOT_H, COL_EDGE);
    if (active) {
        for (int i = 0; i < 5; i++) {
            int y = HEAD_H + 16 + i * 36;
            ui_button(s, 12, y, SIDE_W - 24, 28, nav[i], view == i, hovered(12, y, SIDE_W - 24, 28));
            add_hit(12, y, SIDE_W - 24, 28, A_NAV, i);
            if (i == V_REQUESTS && n_requests) {
                char b[8];
                fmt_u64(b, (uint64_t)n_requests, 10);
                draw_rrect(s, SIDE_W - 46, y + 5, 22, 18, 9, RR_ALL, COL_DANGER, 255);
                draw_text_f(s, SIDE_W - 39, y + 6, b, 0xffffff, FONT_BOLD);
            }
        }
    }
    draw_text_clip(s, 16, win_h - FOOT_H - 46, address[0] ? address : "", COL_MUTED, SIDE_W - 24);
    if (device[0]) {
        draw_text(s, 16, win_h - FOOT_H - 26, "device", 0x5b6676);
        draw_text_f(s, 66, win_h - FOOT_H - 26, device, 0x7b8798, FONT_MONO);
    }

    /* content */
    if (foreign) {
        card(s, cx, cy, cw, 96);
        draw_icon(s, cx + 18, cy + 22, ICON_LOCK);
        draw_text_f(s, cx + 46, cy + 18, "This machine's workspace belongs to another user.", COL_TEXT, FONT_BOLD);
        draw_text_clip(s, cx + 46, cy + 44, "You cannot see its members or files, and they cannot see yours.", COL_MUTED, cw - 64);
    } else if (!active) {
        draw_none(s, cx, cy, cw);
    } else if (view == V_OVERVIEW) {
        draw_overview(s, cx, cy, cw);
    } else if (view == V_MEMBERS) {
        draw_members(s, cx, cy, cw);
    } else if (view == V_FILES) {
        draw_files(s, cx, cy, cw, ch);
    } else if (view == V_REQUESTS) {
        draw_requests(s, cx, cy, cw);
    } else {
        draw_activity(s, cx, cy, cw);
    }

    /* footer */
    draw_fill(s, 0, win_h - FOOT_H, win_w, FOOT_H, COL_PANEL);
    draw_fill(s, 0, win_h - FOOT_H, win_w, 1, COL_EDGE);
    draw_text_clip(s, 12, win_h - FOOT_H + 7, message[0] ? message : "Ready", COL_MUTED, win_w - 330);
    draw_text(s, win_w - 12 - text_width("X25519 + ML-KEM-768  \xc2\xb7  XChaCha20-Poly1305"), win_h - FOOT_H + 7,
              "X25519 + ML-KEM-768  \xc2\xb7  XChaCha20-Poly1305", 0x5b6676);

    if (dialog) {
        nhits = 0; /* the dialog is modal */
        draw_dialog(s);
    }
    ui_present();
}

/* ---- input ------------------------------------------------------------- */

static int hit_at(int x, int y) {
    for (int i = nhits - 1; i >= 0; i--) {
        if (rect_contains(hits[i].r, x, y)) return i;
    }
    return -1;
}

static void key(uint32_t k) {
    char *f = fields[focus_field];
    int len = (int)str_len(f);
    int typing = dialog || (!active && !foreign);
    if (!typing) {
        if (k >= '1' && k <= '5') view = (int)(k - '1');
        else if (k == 'r') refresh();
        return;
    }
    if (k == '\n' || k == '\r') {
        if (dialog) do_add();
        else if (focus_field == F_CREATE) do_create();
        else do_join();
    } else if (k == '\t') {
        if (!dialog) focus_field = focus_field == F_CREATE ? F_CODE : focus_field == F_CODE ? F_NAME : F_CREATE;
    } else if (k == 27) {
        dialog = 0;
    } else if (k == '\b' || k == 127) {
        if (len) f[len - 1] = '\0';
    } else if (k >= 32 && k <= 126 && len < (int)sizeof(fields[0]) - 1) {
        f[len] = (char)k;
        f[len + 1] = '\0';
    }
}

int main(void) {
    ui_event_t ev;
    if (ui_open("Workspace", WANT_W, WANT_H) != 0) return 1;
    win_w = ui_surface()->w;
    win_h = ui_surface()->h;
    refresh();
    render();
    for (;;) {
        int redraw = 0;
        while (ui_poll(&ev)) {
            if (ev.type == UI_EV_CLOSE) {
                ui_close();
                return 0;
            }
            if (ev.type == UI_EV_KEY) {
                key(ev.key);
                redraw = 1;
            } else if (ev.type == UI_EV_MOUSE) {
                uint32_t pressed = ev.buttons & ~prev_buttons;
                int h;
                prev_buttons = ev.buttons;
                mouse_x = ev.x;
                mouse_y = ev.y;
                h = hit_at(mouse_x, mouse_y);
                if (h != hover_hit) {
                    hover_hit = h;
                    redraw = 1;
                }
                if ((pressed & UI_BTN_LEFT) && h >= 0) {
                    perform(hits[h].action, hits[h].arg);
                    redraw = 1;
                }
            }
        }
        /* Live: members come and go, files and requests arrive. */
        if (sh_ticks() - last_refresh >= REFRESH_TICKS && !dialog) {
            refresh();
            redraw = 1;
        }
        if (redraw) render();
        else sh_sleep(2);
    }
}
