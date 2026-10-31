/* Shusha Explorer: file browser with a built-in text editor.
 * Ported from the ICDA Explorer (desktop.c) onto the Shusha window
 * system and drawing library: places sidebar, back/up history, icon
 * grid with paging, new file/folder and go-to dialogs, context menus,
 * properties, and a plain-text editor (Ctrl+S save, Ctrl+X close). */
#include "lib.h"
#include "ui.h"

#define WANT_W 760
#define WANT_H 480
#define PATH_CAP 192
#define NAME_CAP 64
#define MAX_ITEMS 128
#define HISTORY_CAP 16
#define EDIT_CAP 8192

#define TOP_H 78
#define SIDE_W 140
#define STATUS_H 30
#define CELL_W 100
#define CELL_H 84

enum { MODE_BROWSER, MODE_EDITOR };
enum { DLG_NONE, DLG_NEW_FILE, DLG_NEW_FOLDER, DLG_GOTO, DLG_PROPS };
enum { K_UP = 0x100, K_DOWN, K_LEFT, K_RIGHT, K_DELETE };
enum { A_OPEN, A_EDIT, A_PROPS, A_RELOAD, A_NEW_FILE, A_NEW_FOLDER, A_GOTO, A_TERMINAL };

typedef struct {
    char name[NAME_CAP];
    char path[PATH_CAP];
    int is_dir, is_app;
    uint64_t size;
    int readonly;
    int x, y; /* cell origin when visible, else -1 */
} item_t;

/* Actual window size; the WM may shrink the request on small screens. */
static int win_w = WANT_W, win_h = WANT_H;

static item_t items[MAX_ITEMS];
static int item_count, selected = -1, page, cols = 1, visible = 1;
static char cwd[PATH_CAP] = "/home";
static char status[96] = "Ready";
static char listing[8192];
static char history[HISTORY_CAP][PATH_CAP];
static int history_count;

static int mode = MODE_BROWSER;
static int dialog = DLG_NONE;
static char dlg_input[PATH_CAP];
static int dlg_cursor;
static char props_text[3][96];

static char ed_path[PATH_CAP];
static char ed_buf[EDIT_CAP];
static int ed_len, ed_cursor, ed_top, ed_modified;

static int esc_state;
static int mouse_x, mouse_y;
static uint32_t prev_buttons;
static uint64_t last_click_tick;
static int last_click_item = -1;

static menu_t ctx;
static int ctx_actions[MENU_MAX];
static int ctx_open, ctx_x, ctx_y, ctx_item = -1;

/* ---- helpers ----------------------------------------------------------- */

static void set_status(const char *s) {
    str_copy(status, s, sizeof(status));
}

static int ends_with(const char *s, const char *suffix) {
    uint64_t a = str_len(s), b = str_len(suffix);
    return a >= b && str_eq(s + a - b, suffix);
}

static void path_join(char *out, const char *dir, const char *name) {
    str_copy(out, dir, PATH_CAP);
    if (!str_eq(dir, "/")) str_cat(out, "/", PATH_CAP);
    str_cat(out, name, PATH_CAP);
}

static void path_parent(char *out, const char *path) {
    int last = 0;
    str_copy(out, path, PATH_CAP);
    for (int i = 0; out[i]; i++) {
        if (out[i] == '/') last = i;
    }
    out[last ? last : 1] = '\0';
}

static int hit(int mx, int my, int x, int y, int w, int h) {
    return mx >= x && my >= y && mx < x + w && my < y + h;
}

static int valid_name(const char *n) {
    if (!n[0] || str_eq(n, ".") || str_eq(n, "..")) return 0;
    for (; *n; n++) {
        if (*n == '/' || *n < 32 || *n > 126) return 0;
    }
    return 1;
}

/* ---- browser model ----------------------------------------------------- */

static void refresh(void) {
    long n = sh_list_dir(cwd, listing, sizeof(listing));
    int pos = 0;

    item_count = 0;
    if (n < 0) {
        set_status("Could not read folder");
        return;
    }
    while (pos < n && item_count < MAX_ITEMS) {
        item_t *it = &items[item_count];
        int len = 0;
        sh_stat_t st;

        while (pos < n && listing[pos] != '\n' && len < NAME_CAP - 1) it->name[len++] = listing[pos++];
        while (pos < n && listing[pos] != '\n') pos++;
        pos++;
        it->name[len] = '\0';
        if (!len) continue;
        it->is_dir = it->name[len - 1] == '/';
        if (it->is_dir) it->name[len - 1] = '\0';
        path_join(it->path, cwd, it->name);
        it->is_app = ends_with(it->name, ".app") || ends_with(it->name, ".elf");
        it->size = 0;
        it->readonly = 0;
        if (sh_stat(it->path, &st) == 0) {
            it->size = st.size;
            it->readonly = st.readonly;
            it->is_dir = st.type == SH_NODE_DIR;
        }
        item_count++;
    }
    if (selected >= item_count) selected = -1;
    if (page >= item_count) page = 0;
}

static void navigate(const char *path, int remember) {
    sh_stat_t st;
    if (sh_stat(path, &st) != 0 || st.type != SH_NODE_DIR) {
        set_status("Not a folder");
        return;
    }
    if (remember && history_count < HISTORY_CAP) str_copy(history[history_count++], cwd, PATH_CAP);
    str_copy(cwd, path, sizeof(cwd));
    selected = -1;
    page = 0;
    refresh();
    set_status("Ready");
}

static void go_up(void) {
    char parent[PATH_CAP];
    if (str_eq(cwd, "/")) return;
    path_parent(parent, cwd);
    navigate(parent, 1);
}

static void go_back(void) {
    if (!history_count) {
        set_status("No previous folder");
        return;
    }
    history_count--;
    navigate(history[history_count], 0);
}

/* ---- editor model ------------------------------------------------------ */

static int ed_line_start(int p) {
    while (p > 0 && ed_buf[p - 1] != '\n') p--;
    return p;
}

static int ed_line_end(int p) {
    while (p < ed_len && ed_buf[p] != '\n') p++;
    return p;
}

static int ed_row_of(int p) {
    int row = 0;
    for (int i = 0; i < p; i++) {
        if (ed_buf[i] == '\n') row++;
    }
    return row;
}

static int ed_row_start(int row) {
    int p = 0;
    while (row > 0 && p < ed_len) {
        if (ed_buf[p++] == '\n') row--;
    }
    return p;
}

static int ed_rows_visible(void) {
    return (win_h - TOP_H - STATUS_H - 16) / GLYPH_H;
}

static void ed_scroll(void) {
    int row = ed_row_of(ed_cursor), vis = ed_rows_visible();
    if (row < ed_top) ed_top = row;
    if (row >= ed_top + vis) ed_top = row - vis + 1;
}

static void ed_open(const char *path) {
    long n = sh_read(path, ed_buf, sizeof(ed_buf));
    if (n < 0) {
        set_status("Could not open file");
        return;
    }
    ed_len = (int)n;
    ed_cursor = 0;
    ed_top = 0;
    ed_modified = 0;
    str_copy(ed_path, path, sizeof(ed_path));
    mode = MODE_EDITOR;
    set_status("Ctrl+S save   Ctrl+X close");
}

static void ed_save(void) {
    if (sh_write_file(ed_path, ed_buf, (uint64_t)ed_len) < 0) {
        set_status("Save failed (read-only?)");
        return;
    }
    ed_modified = 0;
    set_status("Saved");
}

static void ed_insert(char c) {
    if (ed_len >= EDIT_CAP - 1) return;
    for (int i = ed_len; i > ed_cursor; i--) ed_buf[i] = ed_buf[i - 1];
    ed_buf[ed_cursor++] = c;
    ed_len++;
    ed_modified = 1;
}

static void ed_remove(int at) {
    if (at < 0 || at >= ed_len) return;
    for (int i = at; i < ed_len - 1; i++) ed_buf[i] = ed_buf[i + 1];
    ed_len--;
    ed_modified = 1;
}

static void ed_vertical(int down) {
    int start = ed_line_start(ed_cursor), col = ed_cursor - start, target;
    if (down) {
        int end = ed_line_end(ed_cursor);
        if (end >= ed_len) return;
        target = end + 1;
    } else {
        if (start == 0) return;
        target = ed_line_start(start - 1);
    }
    ed_cursor = target + col;
    if (ed_cursor > ed_line_end(target)) ed_cursor = ed_line_end(target);
}

/* ---- actions ----------------------------------------------------------- */

static void open_dialog(int kind, const char *initial) {
    dialog = kind;
    str_copy(dlg_input, initial, sizeof(dlg_input));
    dlg_cursor = (int)str_len(dlg_input);
}

static void show_props(int idx) {
    char n[24];
    item_t *it = &items[idx];
    str_copy(props_text[0], it->name, sizeof(props_text[0]));
    str_copy(props_text[1], it->is_dir ? "Folder" : (it->is_app ? "Program" : "File"), sizeof(props_text[1]));
    if (!it->is_dir) {
        str_cat(props_text[1], ", ", sizeof(props_text[1]));
        str_cat(props_text[1], fmt_u64(n, it->size, 10), sizeof(props_text[1]));
        str_cat(props_text[1], " bytes", sizeof(props_text[1]));
    }
    str_copy(props_text[2], it->readonly ? "Read-only (system image)" : "Writable", sizeof(props_text[2]));
    dialog = DLG_PROPS;
}

static void open_item(int idx) {
    item_t *it;
    if (idx < 0 || idx >= item_count) {
        set_status("Select an item first");
        return;
    }
    it = &items[idx];
    if (it->is_dir) navigate(it->path, 1);
    else if (it->is_app) set_status(sh_spawn(it->path) < 0 ? "Could not start program" : "Program started");
    else ed_open(it->path);
}

static void dialog_confirm(void) {
    char full[PATH_CAP];
    if (dialog == DLG_GOTO) {
        dialog = DLG_NONE;
        navigate(dlg_input, 1);
        return;
    }
    if (dialog == DLG_PROPS) {
        dialog = DLG_NONE;
        return;
    }
    if (!valid_name(dlg_input)) {
        set_status("That name is not valid");
        return;
    }
    path_join(full, cwd, dlg_input);
    if (dialog == DLG_NEW_FOLDER) set_status(sh_mkdir(full) == 0 ? "Folder created" : "Could not create folder");
    else set_status(sh_create(full) == 0 ? "File created" : "Could not create file");
    dialog = DLG_NONE;
    refresh();
}

static void run_action(int a) {
    switch (a) {
    case A_OPEN: open_item(ctx_item); break;
    case A_EDIT: if (ctx_item >= 0) ed_open(items[ctx_item].path); break;
    case A_PROPS: if (ctx_item >= 0) show_props(ctx_item); break;
    case A_RELOAD: refresh(); set_status("Reloaded"); break;
    case A_NEW_FILE: open_dialog(DLG_NEW_FILE, ""); break;
    case A_NEW_FOLDER: open_dialog(DLG_NEW_FOLDER, ""); break;
    case A_GOTO: open_dialog(DLG_GOTO, cwd); break;
    case A_TERMINAL: (void)sh_spawn("/apps/terminal.app"); break;
    }
}

static void ctx_add(const char *label, int action) {
    ctx_actions[ctx.count] = action;
    ctx.items[ctx.count++] = label;
}

static void ctx_show(int x, int y, int idx) {
    ctx.count = 0;
    ctx_item = idx;
    if (idx >= 0) {
        selected = idx;
        ctx_add("Open", A_OPEN);
        if (!items[idx].is_dir) ctx_add("Edit", A_EDIT);
        ctx_add("Properties", A_PROPS);
    } else {
        ctx_add("Reload", A_RELOAD);
        ctx_add("New File", A_NEW_FILE);
        ctx_add("New Folder", A_NEW_FOLDER);
        ctx_add("Open Path...", A_GOTO);
        ctx_add("Terminal", A_TERMINAL);
    }
    if (x + menu_width(&ctx) > win_w) x = win_w - menu_width(&ctx);
    if (y + menu_height(&ctx) > win_h) y = win_h - menu_height(&ctx);
    ctx_x = x;
    ctx_y = y;
    ctx_open = 1;
}

/* ---- drawing ----------------------------------------------------------- */

static void layout(void) {
    int gx = SIDE_W + 14, gy = TOP_H + 12;
    cols = (win_w - gx - 10) / CELL_W;
    visible = cols * ((win_h - gy - STATUS_H - 34) / CELL_H);
    if (cols < 1) cols = 1;
    if (visible < cols) visible = cols;
    for (int i = 0; i < item_count; i++) items[i].x = items[i].y = -1;
    for (int slot = 0; slot < visible && page + slot < item_count; slot++) {
        items[page + slot].x = gx + (slot % cols) * CELL_W;
        items[page + slot].y = gy + (slot / cols) * CELL_H;
    }
}

/* "Home" is the signed-in user's own folder (the session starts there). */
static char home_dir[PATH_CAP] = "/home";
static const char *places[][2] = {
    { "Home", home_dir }, { "Root", "/" }, { "Apps", "/apps" }, { "Config", "/etc" }, { "Volumes", "/volumes" },
};
#define NPLACES 5

static void draw_chrome(surface_t *s) {
    draw_fill(s, 0, 0, win_w, win_h, COL_BG);
    draw_fill(s, 0, 0, win_w, TOP_H, COL_PANEL);
    draw_fill(s, 0, TOP_H, win_w, 1, COL_EDGE);
    draw_fill(s, 0, win_h - STATUS_H, win_w, STATUS_H, COL_PANEL);
    draw_fill(s, 0, win_h - STATUS_H, win_w, 1, COL_EDGE);
    draw_text_clip(s, 12, win_h - STATUS_H + 7, status, COL_MUTED, win_w - 24);
}

static void draw_browser(surface_t *s) {
    char n[24];
    layout();
    draw_chrome(s);

    draw_round_fill(s, 12, 10, win_w - 24, 26, 5, COL_EDGE);
    draw_round_fill(s, 13, 11, win_w - 26, 24, 4, COL_BG);
    draw_text_clip(s, 22, 15, cwd, COL_TEXT, win_w - 44);
    ui_button(s, 12, 44, 60, 26, "Back", 0, hit(mouse_x, mouse_y, 12, 44, 60, 26) && history_count);
    ui_button(s, 78, 44, 50, 26, "Up", 0, hit(mouse_x, mouse_y, 78, 44, 50, 26));
    ui_button(s, 134, 44, 92, 26, "New File", 0, hit(mouse_x, mouse_y, 134, 44, 92, 26));
    ui_button(s, 232, 44, 104, 26, "New Folder", 0, hit(mouse_x, mouse_y, 232, 44, 104, 26));
    ui_button(s, 342, 44, 92, 26, "Terminal", 0, hit(mouse_x, mouse_y, 342, 44, 92, 26));

    draw_fill(s, 0, TOP_H + 1, SIDE_W, win_h - TOP_H - STATUS_H - 1, 0x121822);
    draw_fill(s, SIDE_W, TOP_H + 1, 1, win_h - TOP_H - STATUS_H - 1, COL_EDGE);
    draw_text(s, 16, TOP_H + 12, "Places", COL_MUTED);
    for (int i = 0; i < NPLACES; i++) {
        int y = TOP_H + 36 + i * 32;
        ui_button(s, 10, y, SIDE_W - 20, 26, places[i][0], str_eq(cwd, places[i][1]),
                  hit(mouse_x, mouse_y, 10, y, SIDE_W - 20, 26));
    }

    for (int i = page; i < item_count && i < page + visible; i++) {
        item_t *it = &items[i];
        int sel = i == selected, hov = hit(mouse_x, mouse_y, it->x, it->y, CELL_W - 8, CELL_H - 6);
        if (sel || hov) draw_round_fill(s, it->x, it->y, CELL_W - 8, CELL_H - 6, 6, sel ? COL_SELECT : COL_PANEL);
        draw_icon(s, it->x + (CELL_W - 8 - ICON_SIZE) / 2, it->y + 3,
                  it->is_dir ? ICON_FOLDER : (it->is_app ? ICON_APP : ICON_FILE));
        {
            int tw = text_width(it->name), maxw = CELL_W - 14;
            int tx = it->x + (tw < maxw ? (CELL_W - 8 - tw) / 2 : 3);
            draw_text_clip(s, tx, it->y + 54, it->name, sel ? COL_TEXT : 0xc8d1dc, maxw);
        }
    }
    if (!item_count) draw_text(s, SIDE_W + 30, TOP_H + 30, "This folder is empty.", COL_MUTED);
    if (page > 0) ui_button(s, win_w - 190, win_h - STATUS_H - 32, 84, 24, "Previous", 0, 0);
    if (page + visible < item_count) ui_button(s, win_w - 98, win_h - STATUS_H - 32, 84, 24, "Next", 0, 0);

    {
        char count[48];
        str_copy(count, fmt_u64(n, (uint64_t)item_count, 10), sizeof(count));
        str_cat(count, " items", sizeof(count));
        draw_text(s, win_w - 12 - text_width(count), win_h - STATUS_H + 7, count, COL_MUTED);
    }
}

static void draw_editor(surface_t *s) {
    int ax = 12, ay = TOP_H + 8, aw = win_w - 24, ah = win_h - TOP_H - STATUS_H - 16;
    int vis = ed_rows_visible(), p = ed_row_start(ed_top), cur_row = ed_row_of(ed_cursor);
    char n[16];

    draw_chrome(s);
    draw_text(s, 14, 12, ed_modified ? "Editing (modified)" : "Editing", COL_MUTED);
    draw_text_clip(s, 14, 34, ed_path, COL_TEXT, win_w - 220);
    ui_button(s, win_w - 186, 22, 80, 28, "Save", 1, 0);
    ui_button(s, win_w - 98, 22, 84, 28, "Close", 0, hit(mouse_x, mouse_y, win_w - 98, 22, 84, 28));
    draw_round_fill(s, ax, ay, aw, ah, 5, COL_EDGE);
    draw_round_fill(s, ax + 1, ay + 1, aw - 2, ah - 2, 4, 0x0b1017);

    for (int r = 0; r < vis && p <= ed_len; r++) {
        int y = ay + 4 + r * GLYPH_H, col = 0, row = ed_top + r;
        fmt_u64(n, (uint64_t)row + 1, 10);
        draw_text(s, ax + 40 - text_width(n), y, n, 0x4b5563);
        while (p < ed_len && ed_buf[p] != '\n') {
            if (col < (aw - 64) / GLYPH_W) draw_char(s, ax + 52 + col * GLYPH_W, y, ed_buf[p], COL_TEXT);
            if (p == ed_cursor) draw_fill(s, ax + 52 + col * GLYPH_W, y + 1, 2, GLYPH_H - 2, COL_ACCENT_HI);
            p++;
            col++;
        }
        if (p == ed_cursor && row == cur_row) draw_fill(s, ax + 52 + col * GLYPH_W, y + 1, 2, GLYPH_H - 2, COL_ACCENT_HI);
        if (p >= ed_len) break;
        p++;
    }
}

static void draw_dialog(surface_t *s) {
    int w = 420, h = dialog == DLG_PROPS ? 150 : 140, x = (win_w - w) / 2, y = (win_h - h) / 2;
    const char *title = dialog == DLG_NEW_FILE ? "New file" : dialog == DLG_NEW_FOLDER ? "New folder"
                      : dialog == DLG_GOTO ? "Open path" : "Properties";

    draw_fill(s, x + 4, y + 4, w, h, 0x05080c);
    draw_round_fill(s, x, y, w, h, 6, COL_ACCENT);
    draw_round_fill(s, x + 1, y + 1, w - 2, h - 2, 5, COL_PANEL);
    draw_text(s, x + 16, y + 14, title, COL_TEXT);
    if (dialog == DLG_PROPS) {
        for (int i = 0; i < 3; i++) draw_text_clip(s, x + 16, y + 44 + i * 22, props_text[i], i ? COL_MUTED : COL_TEXT, w - 32);
        ui_button(s, x + w - 96, y + h - 40, 80, 26, "OK", 1, 0);
        return;
    }
    draw_round_fill(s, x + 16, y + 46, w - 32, 28, 4, COL_EDGE);
    draw_round_fill(s, x + 17, y + 47, w - 34, 26, 3, COL_BG);
    draw_text_clip(s, x + 24, y + 52, dlg_input, COL_TEXT, w - 48);
    draw_fill(s, x + 24 + text_width_n(dlg_input, dlg_cursor, FONT_SANS), y + 51, 2, GLYPH_H + 2, COL_ACCENT_HI);
    ui_button(s, x + w - 186, y + h - 40, 80, 26, "OK", 1, 0);
    ui_button(s, x + w - 96, y + h - 40, 80, 26, "Cancel", 0, 0);
}

static void render(void) {
    surface_t *s = ui_surface();
    if (mode == MODE_EDITOR) draw_editor(s);
    else draw_browser(s);
    if (dialog != DLG_NONE) draw_dialog(s);
    if (ctx_open) menu_draw(s, ctx_x, ctx_y, &ctx, menu_hit(&ctx, ctx_x, ctx_y, mouse_x, mouse_y));
    ui_present();
}

/* ---- input ------------------------------------------------------------- */

static void special_key(int k) {
    if (dialog != DLG_NONE) {
        int len = (int)str_len(dlg_input);
        if (k == K_LEFT && dlg_cursor > 0) dlg_cursor--;
        else if (k == K_RIGHT && dlg_cursor < len) dlg_cursor++;
        else if (k == K_DELETE && dlg_cursor < len) {
            for (int i = dlg_cursor; i < len; i++) dlg_input[i] = dlg_input[i + 1];
        }
        return;
    }
    if (mode == MODE_EDITOR) {
        if (k == K_LEFT && ed_cursor > 0) ed_cursor--;
        else if (k == K_RIGHT && ed_cursor < ed_len) ed_cursor++;
        else if (k == K_UP) ed_vertical(0);
        else if (k == K_DOWN) ed_vertical(1);
        else if (k == K_DELETE) ed_remove(ed_cursor);
        ed_scroll();
        return;
    }
    {
        int step = k == K_LEFT ? -1 : k == K_RIGHT ? 1 : k == K_UP ? -cols : k == K_DOWN ? cols : 0;
        if (!item_count || !step) return;
        selected = selected < 0 ? 0 : selected + step;
        if (selected < 0) selected = 0;
        if (selected >= item_count) selected = item_count - 1;
        while (selected < page) page -= visible;
        while (selected >= page + visible) page += visible;
        if (page < 0) page = 0;
    }
}

/* Arrow keys arrive as ESC [ A..D, Delete as ESC [ 3 ~.  Returns 1 if
 * the byte belonged to a sequence. */
static int escape_seq(uint32_t k) {
    if (esc_state == 0) {
        if (k != 27) return 0;
        esc_state = 1;
        return 1;
    }
    if (esc_state == 1) {
        if (k != '[') {
            esc_state = 0;
            return 0;
        }
        esc_state = 2;
        return 1;
    }
    if (esc_state == 2) {
        esc_state = 0;
        if (k == 'A') special_key(K_UP);
        else if (k == 'B') special_key(K_DOWN);
        else if (k == 'C') special_key(K_RIGHT);
        else if (k == 'D') special_key(K_LEFT);
        else if (k == '3') esc_state = 3;
        return 1;
    }
    esc_state = 0;
    if (k == '~') special_key(K_DELETE);
    return 1;
}

static void key(uint32_t k) {
    if (escape_seq(k)) return;
    if (ctx_open) {
        ctx_open = 0;
        return;
    }
    if (dialog != DLG_NONE) {
        int len = (int)str_len(dlg_input);
        if (k == '\n' || k == '\r') dialog_confirm();
        else if (k == '\b' || k == 127) {
            if (dlg_cursor > 0) {
                for (int i = dlg_cursor - 1; i < len; i++) dlg_input[i] = dlg_input[i + 1];
                dlg_cursor--;
            }
        } else if (k == 'q' - 'a' + 1 || (k == 'c' - 'a' + 1)) {
            dialog = DLG_NONE;
        } else if (k >= 32 && k <= 126 && len < PATH_CAP - 1) {
            for (int i = len + 1; i > dlg_cursor; i--) dlg_input[i] = dlg_input[i - 1];
            dlg_input[dlg_cursor++] = (char)k;
        }
        return;
    }
    if (mode == MODE_EDITOR) {
        if (k == 19) ed_save();                        /* Ctrl+S */
        else if (k == 24) {                            /* Ctrl+X */
            mode = MODE_BROWSER;
            refresh();
            set_status(ed_modified ? "Closed without saving" : "Ready");
        } else if (k == '\b' || k == 127) {
            if (ed_cursor > 0) ed_remove(--ed_cursor);
        } else if (k == '\n' || k == '\r') ed_insert('\n');
        else if (k == '\t') {
            for (int i = 0; i < 4; i++) ed_insert(' ');
        } else if (k >= 32 && k <= 126) ed_insert((char)k);
        ed_scroll();
        return;
    }
    if (k == '\n' || k == '\r') open_item(selected);
    else if (k == '\b' || k == 127) go_up();
    else if (k == 'r') refresh();
    else if (k == 'n') open_dialog(DLG_NEW_FILE, "");
    else if (k == 'f') open_dialog(DLG_NEW_FOLDER, "");
    else if (k == 'g') open_dialog(DLG_GOTO, cwd);
    else if (k == 't') (void)sh_spawn("/apps/terminal.app");
    else if (k == 'e' && selected >= 0 && !items[selected].is_dir) ed_open(items[selected].path);
}

static int item_at(int mx, int my) {
    for (int i = page; i < item_count && i < page + visible; i++) {
        if (items[i].x >= 0 && hit(mx, my, items[i].x, items[i].y, CELL_W - 8, CELL_H - 6)) return i;
    }
    return -1;
}

static void left_click(int mx, int my) {
    if (ctx_open) {
        int i = menu_hit(&ctx, ctx_x, ctx_y, mx, my);
        ctx_open = 0;
        if (i >= 0) run_action(ctx_actions[i]);
        return;
    }
    if (dialog != DLG_NONE) {
        int w = 420, h = dialog == DLG_PROPS ? 150 : 140, x = (win_w - w) / 2, y = (win_h - h) / 2;
        if (dialog == DLG_PROPS) {
            if (hit(mx, my, x + w - 96, y + h - 40, 80, 26)) dialog = DLG_NONE;
        } else if (hit(mx, my, x + w - 186, y + h - 40, 80, 26)) {
            dialog_confirm();
        } else if (hit(mx, my, x + w - 96, y + h - 40, 80, 26)) {
            dialog = DLG_NONE;
        }
        return;
    }
    if (mode == MODE_EDITOR) {
        if (hit(mx, my, win_w - 186, 22, 80, 28)) ed_save();
        else if (hit(mx, my, win_w - 98, 22, 84, 28)) {
            mode = MODE_BROWSER;
            refresh();
            set_status("Ready");
        } else if (my > TOP_H + 12 && my < win_h - STATUS_H - 8) {
            int row = ed_top + (my - TOP_H - 12) / GLYPH_H, col = (mx - 64) / GLYPH_W;
            int start = ed_row_start(row), end = ed_line_end(start);
            if (col < 0) col = 0;
            ed_cursor = start + col > end ? end : start + col;
        }
        return;
    }
    if (hit(mx, my, 12, 44, 60, 26)) go_back();
    else if (hit(mx, my, 78, 44, 50, 26)) go_up();
    else if (hit(mx, my, 134, 44, 92, 26)) open_dialog(DLG_NEW_FILE, "");
    else if (hit(mx, my, 232, 44, 104, 26)) open_dialog(DLG_NEW_FOLDER, "");
    else if (hit(mx, my, 342, 44, 92, 26)) (void)sh_spawn("/apps/terminal.app");
    else if (hit(mx, my, 12, 10, win_w - 24, 26)) open_dialog(DLG_GOTO, cwd);
    else if (mx < SIDE_W && my > TOP_H) {
        for (int i = 0; i < NPLACES; i++) {
            if (hit(mx, my, 10, TOP_H + 36 + i * 32, SIDE_W - 20, 26)) navigate(places[i][1], 1);
        }
    } else if (page > 0 && hit(mx, my, win_w - 190, win_h - STATUS_H - 32, 84, 24)) {
        page -= visible;
        if (page < 0) page = 0;
    } else if (page + visible < item_count && hit(mx, my, win_w - 98, win_h - STATUS_H - 32, 84, 24)) {
        page += visible;
    } else {
        int i = item_at(mx, my);
        uint64_t now = sh_ticks();
        if (i >= 0 && i == last_click_item && now - last_click_tick < 45) {
            open_item(i);
            last_click_item = -1;
            return;
        }
        selected = i;
        last_click_item = i;
        last_click_tick = now;
        if (i >= 0) set_status(items[i].path);
    }
}

/* What the pointer is over; pure mouse moves repaint only when this
 * changes, so hovering costs nothing while the pointer stays put. */
static int hover_key(int mx, int my) {
    static const int bx[5][2] = { { 12, 60 }, { 78, 50 }, { 134, 92 }, { 232, 104 }, { 342, 92 } };
    if (ctx_open) return 1000 + menu_hit(&ctx, ctx_x, ctx_y, mx, my);
    if (mode == MODE_EDITOR) return hit(mx, my, win_w - 98, 22, 84, 28) ? 2000 : 0;
    if (dialog != DLG_NONE) return 0;
    for (int i = 0; i < 5; i++) {
        if (hit(mx, my, bx[i][0], 44, bx[i][1], 26)) return 3000 + i;
    }
    for (int i = 0; i < NPLACES; i++) {
        if (hit(mx, my, 10, TOP_H + 36 + i * 32, SIDE_W - 20, 26)) return 4000 + i;
    }
    return 5000 + item_at(mx, my);
}

int main(int argc, char **argv) {
    ui_event_t ev;

    if (sh_getuid() != 0 && sh_getcwd(home_dir, sizeof(home_dir)) >= 0 && str_starts(home_dir, "/home/")) {
        str_copy(cwd, home_dir, sizeof(cwd));
    } else {
        str_copy(home_dir, "/home", sizeof(home_dir));
    }
    if (argc > 1) str_copy(cwd, argv[1], sizeof(cwd));
    if (ui_open("Explorer", WANT_W, WANT_H) != 0) return 1;
    win_w = ui_surface()->w;
    win_h = ui_surface()->h;
    refresh();
    render();

    for (;;) {
        int redraw = 0;
        ui_wait(&ev);
        do {
            if (ev.type == UI_EV_CLOSE) {
                ui_close();
                return 0;
            }
            if (ev.type == UI_EV_KEY) {
                key(ev.key);
                redraw = 1;
            } else if (ev.type == UI_EV_MOUSE) {
                uint32_t pressed = ev.buttons & ~prev_buttons;
                int before = hover_key(mouse_x, mouse_y);
                prev_buttons = ev.buttons;
                mouse_x = ev.x;
                mouse_y = ev.y;
                if (pressed || hover_key(mouse_x, mouse_y) != before) redraw = 1;
                if (pressed & UI_BTN_LEFT) left_click(ev.x, ev.y);
                else if ((pressed & UI_BTN_RIGHT) && mode == MODE_BROWSER && dialog == DLG_NONE) {
                    ctx_show(ev.x, ev.y, item_at(ev.x, ev.y));
                }
            }
        } while (ui_poll(&ev));
        if (redraw) render();
    }
}
