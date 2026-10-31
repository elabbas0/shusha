/* Code: Shusha's editor, modelled on Visual Studio Code.
 *
 *   code [path | ws:<file id>]
 *
 * Opens files from the user's home and from the encrypted workspace.
 * Workspace files are decrypted by workspaced into this process only;
 * saving sends the new text back, where it is re-encrypted, signed and
 * replicated.  The owner and higher ranks can edit; lower ranks with a
 * grant get a read-only editor.
 *
 * Editing: syntax colouring with language recognition (code_lang.c),
 * Emmet for HTML/CSS (code_emmet.c), suggestions (Emmet, snippets,
 * keywords, words in the file), auto-closing brackets/quotes/tags,
 * smart indentation, line comments, multi-line indent, move/duplicate/
 * delete line, find & replace, go to line, command palette, quick open,
 * undo/redo, minimap, breadcrumbs and a status bar.
 *
 * Keys follow VS Code where the keyboard can express them: Ctrl+S save,
 * Ctrl+N new, Ctrl+W close, Ctrl+Z/Y undo/redo, Ctrl+F find, Ctrl+R
 * replace, Ctrl+G go to line, Ctrl+P quick open, F1 or Ctrl+Shift+P
 * commands, Ctrl+Space suggest, Ctrl+/ comment, Ctrl+D select word,
 * Ctrl+L select line, Ctrl+Shift+K delete line, Alt+Up/Down move line,
 * Shift+Alt+Down duplicate, Ctrl+B sidebar, Ctrl+PgUp/PgDn switch tab,
 * Tab / Shift+Tab indent (Tab also expands Emmet).
 */
#include "lib.h"
#include "ui.h"
#include "wsclient.h"
#include "code_lang.h"
#include "code_emmet.h"

#define WANT_W 1180
#define WANT_H 630
#define ACT_W 44
#define SIDE_W 230
#define TAB_H 34
#define CRUMB_H 22
#define STATUS_H 22
#define GUTTER 58
#define MINIMAP 90
#define LH 18
#define CW GLYPH_W
#define TABSTOP 4

#define DOC_CAP 32768
#define MAX_TABS 8
#define UNDO_MAX 192
#define POOL_CAP 24576
#define MAX_LINES 6000

/* VS Code "Dark+" colours */
#define C_BG       0x1e1e1eu
#define C_SIDE     0x252526u
#define C_ACT      0x333333u
#define C_TABBAR   0x252526u
#define C_TAB      0x2d2d2du
#define C_LINE     0x282828u
#define C_SEL      0x264f78u
#define C_FIND     0x613214u
#define C_GUTTER   0x858585u
#define C_GUTTER_HI 0xc6c6c6u
#define C_STATUS   0x007accu
#define C_WSBAR    0x68217au
#define C_WIDGET   0x252526u
#define C_WIDGET_EDGE 0x454545u
#define C_TEXT     0xd4d4d4u

static const uint32_t tok_col[T_COUNT] = {
    [T_TEXT] = 0xd4d4d4, [T_KEYWORD] = 0x569cd6, [T_CONTROL] = 0xc586c0, [T_TYPE] = 0x4ec9b0,
    [T_STRING] = 0xce9178, [T_NUMBER] = 0xb5cea8, [T_COMMENT] = 0x6a9955, [T_FUNC] = 0xdcdcaa,
    [T_PUNCT] = 0xd4d4d4, [T_PREPROC] = 0xc586c0, [T_TAG] = 0x569cd6, [T_ATTR] = 0x9cdcfe,
    [T_PROP] = 0x9cdcfe, [T_VALUE] = 0xce9178, [T_SELECTOR] = 0xd7ba7d, [T_HEADING] = 0x569cd6,
    [T_EMPH] = 0xc586c0, [T_VARIABLE] = 0x9cdcfe,
};

static const uint32_t lang_col[L_COUNT] = { 0x9aa5b1, 0x6d9cd8, 0xf1dd3f, 0x3e7bb6, 0xe44d26, 0x42a5f5, 0xcbcb41,
                                            0x519aba, 0x89e051 };

typedef struct {
    int pos, del_len, ins_len, cur_before, cur_after;
    uint32_t off;
    uint64_t t;
} uop_t;

typedef struct {
    int used;
    char title[64], path[160], ws_id[33];
    int ws_owner_rank, readonly, lang;
    char buf[DOC_CAP];
    int len, cur, anchor, top, left, dirty, want_col;
    uop_t undo[UNDO_MAX];
    int nundo, nredo;
    char pool[POOL_CAP];
    int pool_used;
} tab_t;

static tab_t tabs[MAX_TABS];
static int active = -1, untitled_n;

/* workspace / home listings for the sidebar */
typedef struct {
    char id[33], name[65], owner[24];
    int owner_rank, open, size;
} wfile_t;
static wfile_t wfiles[48];
static int nwfiles, ws_active, my_rank;
static char ws_name[32], home[96];
static char hfiles[48][64];
static int nhfiles;

static int win_w = WANT_W, win_h = WANT_H;
static int show_side = 1, show_minimap = 1, side_search;
static char message[160];
static uint64_t message_t;
static char clip[DOC_CAP];
static int clip_len, clip_line;

/* line table for the active tab */
static int ls[MAX_LINES], nlines;
static uint8_t lstate[MAX_LINES];
static uint8_t cls[4096];

/* overlays */
enum { OV_NONE, OV_PALETTE, OV_QUICK, OV_GOTO, OV_SAVEAS, OV_WSNEW, OV_LANG };
static int overlay;
static char ov_text[128];
static int ov_sel;
static int find_open, find_focus_replace;
static char find_text[96], repl_text[96];
static int close_armed = -1;

/* suggestions */
enum { S_EMMET, S_SNIPPET, S_KEYWORD, S_WORD };
typedef struct {
    int kind;
    char label[48];
    char insert[512];
} sugg_t;
static sugg_t sugg[12];
static int nsugg, sugg_sel, sugg_open, sugg_from;

static int mouse_x, mouse_y, dragging;
static uint32_t prev_buttons;
static uint64_t last_click_t;
static int last_click_pos = -1;

typedef struct {
    rect_t r;
    int action, arg;
} hit_t;
static hit_t hits[160];
static int nhits;

static char reply[WS_MAX_FILE + 2048];

enum {
    A_NONE, A_TAB, A_TAB_CLOSE, A_WSFILE, A_HOMEFILE, A_ACT_FILES, A_ACT_SEARCH, A_TEXT, A_MINIMAP, A_PALETTE_ITEM,
    A_SUGG, A_FIND_NEXT, A_FIND_PREV, A_FIND_CLOSE, A_REPLACE_ALL, A_STATUS_LANG, A_SEARCH_HIT, A_SIDE_NEW,
    A_SIDE_REFRESH
};

static void set_msg(const char *s) {
    int i = 0;
    while (s[i] && s[i] != '\n' && i < (int)sizeof(message) - 1) {
        message[i] = s[i];
        i++;
    }
    message[i] = '\0';
    message_t = sh_ticks();
}

static tab_t *T(void) {
    return active >= 0 ? &tabs[active] : 0;
}

/* ---- text model --------------------------------------------------------- */

static void reindex(tab_t *t) {
    int st = 0;
    nlines = 0;
    ls[nlines++] = 0;
    for (int i = 0; i < t->len && nlines < MAX_LINES; i++) {
        if (t->buf[i] == '\n') ls[nlines++] = i + 1;
    }
    for (int l = 0; l < nlines; l++) {
        int s = ls[l], e = l + 1 < nlines ? ls[l + 1] - 1 : t->len, n = e - s;
        lstate[l] = (uint8_t)st;
        if (n > (int)sizeof(cls)) n = (int)sizeof(cls);
        st = lang_line(t->lang, t->buf + s, n, st, cls);
    }
}

static int line_of(int pos) {
    int lo = 0, hi = nlines - 1;
    while (lo < hi) {
        int mid = (lo + hi + 1) / 2;
        if (ls[mid] <= pos) lo = mid;
        else hi = mid - 1;
    }
    return lo;
}

static int line_end(tab_t *t, int l) {
    return l + 1 < nlines ? ls[l + 1] - 1 : t->len;
}

static int vcol(tab_t *t, int pos) {
    int l = line_of(pos), c = 0;
    for (int i = ls[l]; i < pos; i++) c = t->buf[i] == '\t' ? (c / TABSTOP + 1) * TABSTOP : c + 1;
    return c;
}

static int pos_at_vcol(tab_t *t, int l, int col) {
    int c = 0, i = ls[l], e = line_end(t, l);
    while (i < e) {
        int w = t->buf[i] == '\t' ? (c / TABSTOP + 1) * TABSTOP - c : 1;
        if (c + w / 2 >= col && col <= c) break;
        if (c + w > col) break;
        c += w;
        i++;
    }
    return i;
}

static int is_word(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '$';
}

static int sel_range(tab_t *t, int *a, int *b) {
    *a = t->anchor < t->cur ? t->anchor : t->cur;
    *b = t->anchor < t->cur ? t->cur : t->anchor;
    return *a != *b;
}

/* The one primitive every edit goes through (records undo). */
static int replace(tab_t *t, int pos, int del, const char *ins, int ins_len, int cur_after) {
    uop_t *op;
    if (t->readonly) {
        set_msg("Read-only: only the file's owner or a higher rank can edit it");
        return -1;
    }
    if (t->len - del + ins_len >= DOC_CAP) {
        set_msg("The file is too large for the editor");
        return -1;
    }
    /* coalesce typing into one undo step */
    op = t->nundo ? &t->undo[t->nundo - 1] : 0;
    t->nredo = 0;
    if (op && del == 0 && ins_len == 1 && ins[0] != '\n' && op->del_len == 0 && pos == op->pos + op->ins_len &&
        sh_ticks() - op->t < 150 && (int)(op->off + op->ins_len) == t->pool_used && t->pool_used + 1 < POOL_CAP) {
        t->pool[t->pool_used++] = ins[0];
        op->ins_len++;
        op->cur_after = cur_after;
        op->t = sh_ticks();
    } else {
        if (t->nundo == UNDO_MAX || t->pool_used + del + ins_len >= POOL_CAP) {
            t->nundo = 0; /* history full: start over */
            t->pool_used = 0;
        }
        if (t->pool_used + del + ins_len < POOL_CAP) {
            op = &t->undo[t->nundo++];
            op->pos = pos;
            op->del_len = del;
            op->ins_len = ins_len;
            op->cur_before = t->cur;
            op->cur_after = cur_after;
            op->off = (uint32_t)t->pool_used;
            op->t = sh_ticks();
            mem_copy(t->pool + t->pool_used, t->buf + pos, (uint64_t)del);
            mem_copy(t->pool + t->pool_used + del, ins, (uint64_t)ins_len);
            t->pool_used += del + ins_len;
        }
    }
    if (ins_len != del) {
        int tail = t->len - pos - del;
        if (ins_len > del) {
            for (int i = tail - 1; i >= 0; i--) t->buf[pos + ins_len + i] = t->buf[pos + del + i];
        } else {
            for (int i = 0; i < tail; i++) t->buf[pos + ins_len + i] = t->buf[pos + del + i];
        }
    }
    mem_copy(t->buf + pos, ins, (uint64_t)ins_len);
    t->len += ins_len - del;
    t->cur = t->anchor = cur_after;
    t->dirty = 1;
    t->want_col = -1;
    reindex(t);
    return 0;
}

static void apply_undo(tab_t *t, int redo) {
    uop_t *op;
    char tmp[DOC_CAP];
    int pos, del, ins_len;
    const char *ins;
    if (redo ? !t->nredo : !t->nundo) return;
    op = redo ? &t->undo[t->nundo] : &t->undo[t->nundo - 1];
    pos = op->pos;
    del = redo ? op->del_len : op->ins_len;
    ins_len = redo ? op->ins_len : op->del_len;
    ins = t->pool + op->off + (redo ? op->del_len : 0);
    mem_copy(tmp, ins, (uint64_t)ins_len);
    {
        int tail = t->len - pos - del;
        if (ins_len > del) for (int i = tail - 1; i >= 0; i--) t->buf[pos + ins_len + i] = t->buf[pos + del + i];
        else for (int i = 0; i < tail; i++) t->buf[pos + ins_len + i] = t->buf[pos + del + i];
    }
    mem_copy(t->buf + pos, tmp, (uint64_t)ins_len);
    t->len += ins_len - del;
    t->cur = t->anchor = redo ? op->cur_after : op->cur_before;
    if (redo) {
        t->nundo++;
        t->nredo--;
    } else {
        t->nundo--;
        t->nredo++;
    }
    t->dirty = 1;
    reindex(t);
}

static void insert_text(tab_t *t, const char *s, int n) {
    int a, b;
    if (sel_range(t, &a, &b)) replace(t, a, b - a, s, n, a + n);
    else replace(t, t->cur, 0, s, n, t->cur + n);
}

/* Insert text that may hold EMMET_CURSOR: the cursor lands there. */
static void insert_with_cursor(tab_t *t, int at, int del, const char *s, int n) {
    char clean[1024];
    int k = 0, mark = -1;
    for (int i = 0; i < n && k < (int)sizeof(clean); i++) {
        if (s[i] == EMMET_CURSOR) {
            if (mark < 0) mark = k;
        } else {
            clean[k++] = s[i];
        }
    }
    replace(t, at, del, clean, k, at + (mark >= 0 ? mark : k));
}

static int indent_of(tab_t *t, int l, char *out, int cap) {
    int i = ls[l], e = line_end(t, l), n = 0;
    while (i < e && (t->buf[i] == ' ' || t->buf[i] == '\t') && n < cap - 1) out[n++] = t->buf[i++];
    out[n] = '\0';
    return n;
}

/* ---- files -------------------------------------------------------------- */

static int find_tab(const char *path, const char *ws_id) {
    for (int i = 0; i < MAX_TABS; i++) {
        if (!tabs[i].used) continue;
        if (ws_id && ws_id[0] && str_eq(tabs[i].ws_id, ws_id)) return i;
        if (path && path[0] && str_eq(tabs[i].path, path)) return i;
    }
    return -1;
}

static tab_t *new_tab(const char *title) {
    for (int i = 0; i < MAX_TABS; i++) {
        if (!tabs[i].used) {
            tab_t *t = &tabs[i];
            mem_set(t, 0, sizeof(*t));
            t->used = 1;
            t->want_col = -1;
            str_copy(t->title, title, sizeof(t->title));
            active = i;
            return t;
        }
    }
    set_msg("Too many open editors - close one first");
    return 0;
}

static const char *base_name(const char *p) {
    const char *b = p;
    for (; *p; p++) {
        if (*p == '/') b = p + 1;
    }
    return b;
}

static void open_home(const char *path) {
    int i = find_tab(path, 0);
    tab_t *t;
    long n;
    if (i >= 0) {
        active = i;
        reindex(T());
        return;
    }
    if (!(t = new_tab(base_name(path)))) return;
    str_copy(t->path, path, sizeof(t->path));
    n = sh_read_at(path, 0, t->buf, DOC_CAP - 1);
    if (n < 0) {
        t->used = 0;
        active = -1;
        set_msg("Cannot open that file");
        return;
    }
    t->len = (int)n;
    t->lang = lang_detect(t->title, t->buf, t->len);
    reindex(t);
    set_msg(lang_name(t->lang));
}

static void open_ws(int k) {
    char line[48];
    int failed, i = find_tab(0, wfiles[k].id);
    tab_t *t;
    long n;
    if (i >= 0) {
        active = i;
        reindex(T());
        return;
    }
    if (!wfiles[k].open) {
        set_msg("Locked: your rank cannot open this file - ask for access in the Workspace app");
        return;
    }
    str_copy(line, "raw #", sizeof(line));
    str_cat(line, wfiles[k].id, sizeof(line));
    n = ws_request(line, 0, 0, reply, sizeof(reply), &failed);
    if (n < 0 || failed) {
        set_msg(reply);
        return;
    }
    if (!(t = new_tab(wfiles[k].name))) return;
    str_copy(t->ws_id, wfiles[k].id, sizeof(t->ws_id));
    t->ws_owner_rank = wfiles[k].owner_rank;
    t->readonly = my_rank > wfiles[k].owner_rank;
    mem_copy(t->buf, reply, (uint64_t)n);
    t->len = (int)n;
    mem_wipe(reply, (uint64_t)n);
    t->lang = lang_detect(t->title, t->buf, t->len);
    reindex(t);
    set_msg(t->readonly ? "Opened read-only (you can view, the owner or a higher rank can edit)"
                        : "Decrypted in memory only - saving re-encrypts and replicates it");
}

static void load_lists(void) {
    char *p;
    nwfiles = ws_active = 0;
    if (ws_request("q status", 0, 0, reply, sizeof(reply), 0) >= 0) {
        for (p = reply; *p;) {
            if (str_starts(p, "active\t1")) ws_active = 1;
            if (str_starts(p, "name\t")) {
                int k = 0;
                p += 5;
                while (*p && *p != '\n' && k < 31) ws_name[k++] = *p++;
                ws_name[k] = '\0';
            }
            if (str_starts(p, "rank\t")) my_rank = p[5] - '0';
            while (*p && *p != '\n') p++;
            if (*p) p++;
        }
    }
    if (ws_active && ws_request("q files", 0, 0, reply, sizeof(reply), 0) >= 0) {
        for (p = reply; *p && nwfiles < 48;) {
            wfile_t *f = &wfiles[nwfiles++];
            char *fields[7];
            int nf = 0;
            for (char *q = p; nf < 7;) {
                fields[nf++] = q;
                while (*q && *q != '\t' && *q != '\n') q++;
                if (*q == '\t') *q++ = '\0';
                else {
                    if (*q == '\n') *q++ = '\0';
                    p = q;
                    break;
                }
            }
            if (nf < 6) {
                nwfiles--;
                continue;
            }
            str_copy(f->id, fields[0], sizeof(f->id));
            str_copy(f->name, fields[1], sizeof(f->name));
            f->owner_rank = fields[2][0] - '0';
            str_copy(f->owner, fields[3], sizeof(f->owner));
            f->open = str_eq(fields[4], "open");
        }
    }
    nhfiles = 0;
    {
        char list[2048];
        long n = sh_list_dir(home, list, sizeof(list));
        for (long i = 0; n > 0 && i < n && nhfiles < 48;) {
            int k = 0;
            while (i < n && list[i] != '\n' && k < 63) hfiles[nhfiles][k++] = list[i++];
            hfiles[nhfiles][k] = '\0';
            while (i < n && list[i] != '\n') i++;
            i++;
            if (k && hfiles[nhfiles][k - 1] != '/') nhfiles++;
        }
    }
}

static void save(void) {
    tab_t *t = T();
    if (!t) return;
    if (t->ws_id[0]) {
        char line[48];
        str_copy(line, "update #", sizeof(line));
        str_cat(line, t->ws_id, sizeof(line));
        if (ws_request(line, t->buf, (uint64_t)t->len, reply, sizeof(reply), 0) >= 0 && str_starts(reply, "saved")) {
            t->dirty = 0;
            /* "saved x (version n, encrypted)\nreplicated to a of b" */
            for (char *p = reply; *p; p++) {
                if (*p == '\n' && p[1]) *p = ' ';
            }
        }
        set_msg(reply);
        return;
    }
    if (!t->path[0]) {
        overlay = OV_SAVEAS;
        str_copy(ov_text, t->title, sizeof(ov_text));
        return;
    }
    if (sh_write_file(t->path, t->buf, (uint64_t)t->len) < 0) {
        set_msg("Could not save (no permission?)");
        return;
    }
    t->dirty = 0;
    set_msg("Saved");
}

static void save_as(const char *name) {
    tab_t *t = T();
    char path[160];
    if (!t || !name[0]) return;
    str_copy(path, home, sizeof(path));
    str_cat(path, "/", sizeof(path));
    str_cat(path, name, sizeof(path));
    (void)sh_create(path);
    (void)sh_chmod(path, 0600);
    str_copy(t->path, path, sizeof(t->path));
    str_copy(t->title, name, sizeof(t->title));
    t->lang = lang_detect(name, t->buf, t->len);
    reindex(t);
    save();
    load_lists();
}

/* Share the editor's text as a new workspace file (owned by you). */
static void save_to_workspace(const char *name) {
    tab_t *t = T();
    char line[96];
    if (!t || !name[0]) return;
    if (!ws_active) {
        set_msg("You are not in a workspace");
        return;
    }
    str_copy(line, "put ", sizeof(line));
    str_cat(line, name, sizeof(line));
    ws_request(line, t->buf, (uint64_t)t->len, reply, sizeof(reply), 0);
    set_msg(reply);
    load_lists();
    for (int k = nwfiles - 1; k >= 0; k--) {
        if (str_eq(wfiles[k].name, name) && wfiles[k].owner_rank == my_rank) {
            str_copy(t->ws_id, wfiles[k].id, sizeof(t->ws_id));
            str_copy(t->title, name, sizeof(t->title));
            t->path[0] = '\0';
            t->ws_owner_rank = my_rank;
            t->readonly = 0;
            t->dirty = 0;
            t->lang = lang_detect(name, t->buf, t->len);
            reindex(t);
            break;
        }
    }
}

static void new_file(void) {
    char title[32], n[8];
    tab_t *t;
    str_copy(title, "Untitled-", sizeof(title));
    str_cat(title, fmt_u64(n, (uint64_t)++untitled_n, 10), sizeof(title));
    if ((t = new_tab(title))) {
        t->lang = L_PLAIN;
        reindex(t);
    }
}

static void close_tab(int i) {
    if (i < 0 || !tabs[i].used) return;
    if (tabs[i].dirty && close_armed != i) {
        close_armed = i;
        set_msg("Unsaved changes - close again to discard them, or Ctrl+S to save");
        return;
    }
    close_armed = -1;
    mem_wipe(tabs[i].buf, sizeof(tabs[i].buf));
    mem_wipe(tabs[i].pool, sizeof(tabs[i].pool));
    tabs[i].used = 0;
    if (active == i) {
        active = -1;
        for (int k = MAX_TABS - 1; k >= 0; k--) {
            if (tabs[k].used) {
                active = k;
                break;
            }
        }
    }
    if (T()) reindex(T());
}

/* ---- view ---------------------------------------------------------------- */

static int ed_x(void) {
    return ACT_W + (show_side ? SIDE_W : 0);
}

static int ed_y(void) {
    return TAB_H + CRUMB_H;
}

static int ed_h(void) {
    return win_h - ed_y() - STATUS_H;
}

static int text_x(void) {
    return ed_x() + GUTTER;
}

static int text_w(void) {
    return win_w - text_x() - (show_minimap ? MINIMAP : 0) - 12;
}

static int rows(void) {
    return ed_h() / LH;
}

static void scroll_to_cursor(tab_t *t) {
    int l = line_of(t->cur), c = vcol(t, t->cur), cols = text_w() / CW - 1;
    if (l < t->top) t->top = l;
    if (l >= t->top + rows() - 1) t->top = l - rows() + 2;
    if (t->top < 0) t->top = 0;
    if (c < t->left) t->left = c;
    if (c > t->left + cols) t->left = c - cols;
}

/* ---- suggestions ---------------------------------------------------------- */

static const struct { int lang; const char *trigger, *label, *body; } snippets[] = {
    { L_C, "for", "for loop", "for (int i = 0; i < \x01n; i++) {\n\t\n}" },
    { L_C, "if", "if statement", "if (\x01) {\n\t\n}" },
    { L_C, "main", "main function", "int main(int argc, char **argv) {\n\t\x01\n\treturn 0;\n}" },
    { L_C, "inc", "#include", "#include <\x01.h>" },
    { L_C, "struct", "typedef struct", "typedef struct {\n\t\x01\n} name_t;" },
    { L_JS, "log", "console.log", "console.log(\x01);" },
    { L_JS, "fn", "function", "function \x01name(params) {\n\t\n}" },
    { L_JS, "af", "arrow function", "const \x01name = (params) => {\n\t\n};" },
    { L_JS, "for", "for loop", "for (let i = 0; i < \x01" "array.length; i++) {\n\t\n}" },
    { L_JS, "forof", "for...of", "for (const \x01item of items) {\n\t\n}" },
    { L_JS, "if", "if statement", "if (\x01) {\n\t\n}" },
    { L_JS, "imp", "import", "import \x01name from 'module';" },
    { L_PY, "def", "function", "def \x01name(args):\n\tpass" },
    { L_PY, "class", "class", "class \x01Name:\n\tdef __init__(self):\n\t\tpass" },
    { L_PY, "for", "for loop", "for \x01item in items:\n\tpass" },
    { L_PY, "ifmain", "if __name__ == \"__main__\"", "if __name__ == \"__main__\":\n\t\x01main()" },
    { L_PY, "with", "with open", "with open(\x01path) as f:\n\tdata = f.read()" },
    { L_SH, "if", "if statement", "if [ \x01 ]; then\n\t\nfi" },
    { L_SH, "for", "for loop", "for \x01x in list; do\n\t\ndone" },
    { L_CSS, "media", "@media", "@media (max-width: \x01" "768px) {\n\t\n}" },
    { L_MD, "code", "code block", "```\x01\n\n```" },
    { L_MD, "link", "link", "[\x01text](url)" },
};

/* Replace \t in a snippet by the line's indentation plus one level. */
static int expand_tabs(const char *body, const char *indent, char *out, int cap) {
    int k = 0;
    for (const char *p = body; *p && k < cap - 8; p++) {
        if (*p == '\n') {
            out[k++] = '\n';
            for (const char *q = indent; *q && k < cap - 8; q++) out[k++] = *q;
        } else if (*p == '\t') {
            for (int s = 0; s < TABSTOP; s++) out[k++] = ' ';
        } else {
            out[k++] = *p;
        }
    }
    out[k] = '\0';
    return k;
}

static void add_sugg(int kind, const char *label, const char *ins) {
    for (int i = 0; i < nsugg; i++) {
        if (str_eq(sugg[i].label, label)) return;
    }
    if (nsugg < 12) {
        sugg[nsugg].kind = kind;
        str_copy(sugg[nsugg].label, label, sizeof(sugg[nsugg].label));
        str_copy(sugg[nsugg].insert, ins, sizeof(sugg[nsugg].insert));
        nsugg++;
    }
}

static void build_suggestions(int explicit) {
    tab_t *t = T();
    int l, s, p, n;
    char prefix[48], indent[64];
    nsugg = sugg_sel = 0;
    sugg_open = 0;
    if (!t || t->readonly) return;
    l = line_of(t->cur);
    p = t->cur;
    s = p;
    while (s > ls[l] && is_word(t->buf[s - 1])) s--;
    n = p - s;
    if (n >= (int)sizeof(prefix)) return;
    mem_copy(prefix, t->buf + s, (uint64_t)n);
    prefix[n] = '\0';
    indent_of(t, l, indent, sizeof(indent));
    sugg_from = s;
    /* Emmet: the whole abbreviation before the cursor */
    if (lang_emmet(t->lang)) {
        char line[512], out[512];
        int ll = p - ls[l] < 511 ? p - ls[l] : 511, a;
        mem_copy(line, t->buf + ls[l], (uint64_t)ll);
        line[ll] = '\0';
        a = emmet_abbr_start(t->lang == L_CSS, line, ll);
        if (a >= 0 && (ll - a >= 2 || explicit) &&
            emmet_expand(t->lang == L_CSS, line + a, ll - a, indent, "    ", out, sizeof(out)) > 0) {
            char label[48];
            int k = ll - a < 40 ? ll - a : 40;
            mem_copy(label, line + a, (uint64_t)k);
            label[k] = '\0';
            add_sugg(S_EMMET, label, out);
            sugg_from = ls[l] + a;
        }
    }
    if (n < 1 && !explicit) return;
    for (unsigned i = 0; i < sizeof(snippets) / sizeof(snippets[0]); i++) {
        if (snippets[i].lang == t->lang && str_starts(snippets[i].trigger, prefix)) {
            char body[512];
            expand_tabs(snippets[i].body, indent, body, sizeof(body));
            add_sugg(S_SNIPPET, snippets[i].trigger, body);
            str_copy(sugg[nsugg - 1].label, snippets[i].trigger, sizeof(sugg[0].label));
        }
    }
    for (const char *const *w = lang_words(t->lang); *w; w++) {
        if (str_starts(*w, prefix) && !str_eq(*w, prefix)) add_sugg(S_KEYWORD, *w, *w);
    }
    /* words already in the file */
    for (int i = 0; i < t->len && nsugg < 12;) {
        int j = i;
        if (!is_word(t->buf[i])) {
            i++;
            continue;
        }
        while (j < t->len && is_word(t->buf[j])) j++;
        if (j - i > n && j - i < 40 && j - i >= 3 && i != s) {
            char w[48];
            mem_copy(w, t->buf + i, (uint64_t)(j - i));
            w[j - i] = '\0';
            if (str_starts(w, prefix) && n) add_sugg(S_WORD, w, w);
        }
        i = j;
    }
    /* an Emmet entry keeps the whole abbreviation as its range; others
     * replace only the word */
    if (nsugg && sugg[0].kind != S_EMMET) sugg_from = s;
    sugg_open = nsugg > 0;
}

static void accept_suggestion(void) {
    tab_t *t = T();
    sugg_t *g;
    int from;
    if (!t || !sugg_open || sugg_sel >= nsugg) return;
    g = &sugg[sugg_sel];
    from = sugg_from;
    if (g->kind != S_EMMET) {
        from = t->cur;
        while (from > 0 && is_word(t->buf[from - 1])) from--;
    }
    insert_with_cursor(t, from, t->cur - from, g->insert, (int)str_len(g->insert));
    sugg_open = 0;
}

/* Tab: expand an Emmet abbreviation before the cursor, if there is one. */
static int try_emmet(tab_t *t) {
    char line[512], out[1024], indent[64];
    int l = line_of(t->cur), ll = t->cur - ls[l], a, n;
    if (!lang_emmet(t->lang) || ll <= 0 || ll >= 511) return 0;
    if (t->lang == L_HTML && (lstate[l] == 1 || lstate[l] == 2)) return 0;
    mem_copy(line, t->buf + ls[l], (uint64_t)ll);
    line[ll] = '\0';
    a = emmet_abbr_start(t->lang == L_CSS, line, ll);
    if (a < 0) return 0;
    indent_of(t, l, indent, sizeof(indent));
    n = emmet_expand(t->lang == L_CSS, line + a, ll - a, indent, "    ", out, sizeof(out));
    if (n <= 0) return 0;
    insert_with_cursor(t, ls[l] + a, ll - a, out, n);
    set_msg("Emmet: expanded abbreviation");
    return 1;
}

/* ---- editing commands -------------------------------------------------------- */

static int line_has_sel_lines(tab_t *t, int *l0, int *l1) {
    int a, b;
    sel_range(t, &a, &b);
    *l0 = line_of(a);
    *l1 = line_of(b > a && b == ls[line_of(b)] ? b - 1 : b);
    return *l1 > *l0;
}

static void indent_lines(tab_t *t, int out) {
    int l0, l1, cur_line, a_line;
    char buf[DOC_CAP];
    int k = 0;
    line_has_sel_lines(t, &l0, &l1);
    cur_line = line_of(t->cur);
    a_line = line_of(t->anchor);
    for (int l = l0; l <= l1; l++) {
        int s = ls[l], e = line_end(t, l);
        if (out) {
            int r = 0;
            while (r < TABSTOP && s + r < e && t->buf[s + r] == ' ') r++;
            if (r == 0 && s < e && t->buf[s] == '\t') r = 1;
            s += r;
        } else if (e > s) {
            for (int i = 0; i < TABSTOP; i++) buf[k++] = ' ';
        }
        mem_copy(buf + k, t->buf + s, (uint64_t)(e - s));
        k += e - s;
        if (l < l1) buf[k++] = '\n';
    }
    {
        int start = ls[l0], end = line_end(t, l1);
        replace(t, start, end - start, buf, k, start);
        reindex(t);
        t->anchor = ls[a_line < cur_line ? l0 : l1] + (a_line < cur_line ? 0 : line_end(t, l1) - ls[l1]);
        t->cur = a_line < cur_line ? line_end(t, l1) : ls[l0];
        if (l0 == l1) t->anchor = t->cur = line_end(t, l0) < t->cur ? line_end(t, l0) : t->cur;
    }
}

static void toggle_comment(tab_t *t) {
    const char *open, *close;
    int l0, l1, all = 1;
    char buf[DOC_CAP];
    int k = 0;
    lang_comment(t->lang, &open, &close);
    if (!open) {
        set_msg("No comments in this language");
        return;
    }
    line_has_sel_lines(t, &l0, &l1);
    for (int l = l0; l <= l1; l++) {
        int s = ls[l], e = line_end(t, l);
        while (s < e && (t->buf[s] == ' ' || t->buf[s] == '\t')) s++;
        if (s < e) {
            int ol = (int)str_len(open) - 1, ok = 1;
            for (int i = 0; i < ol; i++) ok &= s + i < e && t->buf[s + i] == open[i];
            if (!ok) all = 0;
        }
    }
    for (int l = l0; l <= l1; l++) {
        int s = ls[l], e = line_end(t, l), ws = s;
        while (ws < e && (t->buf[ws] == ' ' || t->buf[ws] == '\t')) ws++;
        mem_copy(buf + k, t->buf + s, (uint64_t)(ws - s));
        k += ws - s;
        if (ws == e) {
            /* blank line stays */
        } else if (all) {
            int ol = (int)str_len(open), cl = close ? (int)str_len(close) : 0, b = ws, en = e;
            if (en - b >= ol && t->buf[b + ol - 1] == ' ') b += ol;
            else b += ol - 1;
            if (cl) {
                if (en - b >= cl && mem_eq_ct(t->buf + en - cl, close, (uint64_t)cl)) en -= cl;
                else if (en - b >= cl - 1 && mem_eq_ct(t->buf + en - cl + 1, close + 1, (uint64_t)cl - 1)) en -= cl - 1;
            }
            mem_copy(buf + k, t->buf + b, (uint64_t)(en - b));
            k += en - b;
        } else {
            mem_copy(buf + k, open, str_len(open));
            k += (int)str_len(open);
            mem_copy(buf + k, t->buf + ws, (uint64_t)(e - ws));
            k += e - ws;
            if (close) {
                mem_copy(buf + k, close, str_len(close));
                k += (int)str_len(close);
            }
        }
        if (l < l1) buf[k++] = '\n';
    }
    {
        int start = ls[l0], end = line_end(t, l1);
        replace(t, start, end - start, buf, k, start + k);
    }
}

static void delete_line(tab_t *t) {
    int l = line_of(t->cur), s = ls[l], e = l + 1 < nlines ? ls[l + 1] : t->len;
    if (e == t->len && s > 0 && l + 1 >= nlines) s--;
    replace(t, s, e - s, "", 0, s);
}

static void move_line(tab_t *t, int down) {
    int l = line_of(t->cur), col = t->cur - ls[l], s, e, os, oe;
    char a[4096], b[4096], buf[8200];
    int na, nb, k = 0;
    if (down ? l + 1 >= nlines : l == 0) return;
    s = ls[l];
    e = line_end(t, l);
    os = ls[down ? l + 1 : l - 1];
    oe = line_end(t, down ? l + 1 : l - 1);
    na = e - s < 4095 ? e - s : 4095;
    nb = oe - os < 4095 ? oe - os : 4095;
    mem_copy(a, t->buf + s, (uint64_t)na);
    mem_copy(b, t->buf + os, (uint64_t)nb);
    if (down) {
        mem_copy(buf, b, (uint64_t)nb), k = nb, buf[k++] = '\n', mem_copy(buf + k, a, (uint64_t)na), k += na;
        replace(t, s, oe - s, buf, k, s + nb + 1 + col);
    } else {
        mem_copy(buf, a, (uint64_t)na), k = na, buf[k++] = '\n', mem_copy(buf + k, b, (uint64_t)nb), k += nb;
        replace(t, os, e - os, buf, k, os + col);
    }
}

static void duplicate_line(tab_t *t) {
    int l = line_of(t->cur), s = ls[l], e = line_end(t, l), col = t->cur - s;
    char buf[4100];
    int n = e - s < 4096 ? e - s : 4096;
    buf[0] = '\n';
    mem_copy(buf + 1, t->buf + s, (uint64_t)n);
    replace(t, e, 0, buf, n + 1, e + 1 + col);
}

static void select_word(tab_t *t) {
    int a = t->cur, b = t->cur;
    while (a > 0 && is_word(t->buf[a - 1])) a--;
    while (b < t->len && is_word(t->buf[b])) b++;
    t->anchor = a;
    t->cur = b;
}

static void copy_sel(tab_t *t, int cut) {
    int a, b;
    if (sel_range(t, &a, &b)) {
        clip_len = b - a;
        mem_copy(clip, t->buf + a, (uint64_t)clip_len);
        clip_line = 0;
        if (cut) replace(t, a, b - a, "", 0, a);
    } else {
        /* no selection: the whole line, like VS Code */
        int l = line_of(t->cur), s = ls[l], e = l + 1 < nlines ? ls[l + 1] : t->len;
        clip_len = e - s;
        mem_copy(clip, t->buf + s, (uint64_t)clip_len);
        if (e == t->len) clip[clip_len++] = '\n';
        clip_line = 1;
        if (cut) delete_line(t);
    }
    set_msg(cut ? "Cut" : "Copied");
}

static void paste(tab_t *t) {
    if (!clip_len) return;
    if (clip_line && t->anchor == t->cur) {
        int s = ls[line_of(t->cur)];
        replace(t, s, 0, clip, clip_len, t->cur + clip_len);
    } else {
        insert_text(t, clip, clip_len);
    }
}

static const char *closer(char c) {
    switch (c) {
    case '(': return ")";
    case '[': return "]";
    case '{': return "}";
    case '"': return "\"";
    case '\'': return "'";
    case '`': return "`";
    }
    return 0;
}

static int is_void_tag(const char *t) {
    static const char *const v[] = { "area", "base", "br", "col", "embed", "hr", "img", "input", "link", "meta",
                                     "source", "track", "wbr", 0 };
    for (int i = 0; v[i]; i++) {
        if (str_eq(v[i], t)) return 1;
    }
    return 0;
}

static void type_char(tab_t *t, char c) {
    int a, b, has_sel = sel_range(t, &a, &b);
    char next = t->cur < t->len ? t->buf[t->cur] : 0, prev = t->cur > 0 ? t->buf[t->cur - 1] : 0;
    const char *cl = closer(c);
    /* type over an auto-inserted closer */
    if (!has_sel && (c == ')' || c == ']' || c == '}' || c == '"' || c == '\'' || c == '`') && next == c) {
        t->cur = t->anchor = t->cur + 1;
        return;
    }
    if (has_sel && cl) { /* wrap the selection */
        char buf[DOC_CAP];
        int n = b - a;
        buf[0] = c;
        mem_copy(buf + 1, t->buf + a, (uint64_t)n);
        buf[n + 1] = cl[0];
        replace(t, a, n, buf, n + 2, a + n + 2);
        return;
    }
    if (cl && (next == 0 || next == ' ' || next == '\n' || next == ')' || next == ']' || next == '}' || next == ';' ||
               next == ',') &&
        !((c == '\'' || c == '"' || c == '`') && (is_word(prev) || t->lang == L_MD || t->lang == L_PLAIN))) {
        char pair[2] = { c, cl[0] };
        replace(t, t->cur, 0, pair, 2, t->cur + 1);
        return;
    }
    if (c == '>' && t->lang == L_HTML) {
        /* auto-close the tag just opened */
        int l = line_of(t->cur), i = t->cur - 1;
        while (i >= ls[l] && t->buf[i] != '<' && t->buf[i] != '>') i--;
        if (i >= ls[l] && t->buf[i] == '<' && i + 1 < t->cur && t->buf[i + 1] != '/' && t->buf[i + 1] != '!' &&
            t->buf[t->cur - 1] != '/') {
            char name[32], ins[48];
            int k = 0, j = i + 1;
            while (j < t->cur && k < 31 && (is_word(t->buf[j]) || t->buf[j] == '-')) name[k++] = t->buf[j++];
            name[k] = '\0';
            if (k && !is_void_tag(name)) {
                str_copy(ins, "></", sizeof(ins));
                str_cat(ins, name, sizeof(ins));
                str_cat(ins, ">", sizeof(ins));
                replace(t, t->cur, 0, ins, (int)str_len(ins), t->cur + 1);
                return;
            }
        }
    }
    insert_text(t, &c, 1);
}

static void enter(tab_t *t) {
    int l = line_of(t->cur);
    char indent[128], buf[300];
    int n = indent_of(t, l, indent, sizeof(indent)), k = 0;
    char prev = t->cur > ls[l] ? t->buf[t->cur - 1] : 0, next = t->cur < t->len ? t->buf[t->cur] : 0;
    int open = prev == '{' || prev == '[' || prev == '(' || (t->lang == L_PY && prev == ':') ||
               (t->lang == L_HTML && prev == '>' && next == '<' && t->cur + 1 < t->len && t->buf[t->cur + 1] == '/');
    buf[k++] = '\n';
    mem_copy(buf + k, indent, (uint64_t)n);
    k += n;
    if (open) {
        for (int i = 0; i < TABSTOP; i++) buf[k++] = ' ';
    }
    if (open && ((prev == '{' && next == '}') || (prev == '[' && next == ']') || (prev == '(' && next == ')') ||
                 (t->lang == L_HTML && next == '<'))) {
        int at = k;
        buf[k++] = '\n';
        mem_copy(buf + k, indent, (uint64_t)n);
        k += n;
        {
            int a, b;
            if (sel_range(t, &a, &b)) replace(t, a, b - a, buf, k, a + at);
            else replace(t, t->cur, 0, buf, k, t->cur + at);
        }
        return;
    }
    insert_text(t, buf, k);
}

static void backspace(tab_t *t) {
    int a, b;
    if (sel_range(t, &a, &b)) {
        replace(t, a, b - a, "", 0, a);
        return;
    }
    if (t->cur == 0) return;
    {
        char p = t->buf[t->cur - 1], n = t->cur < t->len ? t->buf[t->cur] : 0;
        const char *cl = closer(p);
        if (cl && n == cl[0]) {
            replace(t, t->cur - 1, 2, "", 0, t->cur - 1);
            return;
        }
        /* a whole indentation step of spaces */
        {
            int l = line_of(t->cur), s = ls[l], all = 1, c = vcol(t, t->cur);
            for (int i = s; i < t->cur; i++) all &= t->buf[i] == ' ';
            if (all && c > 0) {
                int del = c % TABSTOP ? c % TABSTOP : TABSTOP;
                replace(t, t->cur - del, del, "", 0, t->cur - del);
                return;
            }
        }
    }
    replace(t, t->cur - 1, 1, "", 0, t->cur - 1);
}

/* ---- find ---------------------------------------------------------------- */

static int match_at(tab_t *t, int i) {
    int n = (int)str_len(find_text);
    if (!n || i + n > t->len) return 0;
    for (int k = 0; k < n; k++) {
        char a = t->buf[i + k], b = find_text[k];
        if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
        if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
        if (a != b) return 0;
    }
    return 1;
}

static int count_matches(tab_t *t, int *index) {
    int n = 0, cur_i = 0;
    for (int i = 0; i < t->len; i++) {
        if (match_at(t, i)) {
            n++;
            if (i < t->cur) cur_i = n;
        }
    }
    if (index) *index = cur_i;
    return n;
}

static void find_step(tab_t *t, int back) {
    int n = (int)str_len(find_text);
    if (!n) return;
    for (int k = 1; k <= t->len; k++) {
        int i = back ? (t->anchor < t->cur ? t->anchor : t->cur) - k : (t->anchor > t->cur ? t->anchor : t->cur) + k - 1;
        i = ((i % t->len) + t->len) % t->len;
        if (match_at(t, i)) {
            t->anchor = i;
            t->cur = i + n;
            scroll_to_cursor(t);
            return;
        }
    }
    set_msg("No results");
}

static void replace_one(tab_t *t) {
    int a, b, n = (int)str_len(find_text);
    if (sel_range(t, &a, &b) && b - a == n && match_at(t, a)) {
        replace(t, a, n, repl_text, (int)str_len(repl_text), a + (int)str_len(repl_text));
    }
    find_step(t, 0);
}

static void replace_all(tab_t *t) {
    int n = (int)str_len(find_text), count = 0;
    char num[16], m[48];
    if (!n) return;
    for (int i = 0; i + n <= t->len; i++) {
        if (match_at(t, i)) {
            replace(t, i, n, repl_text, (int)str_len(repl_text), i + (int)str_len(repl_text));
            i += (int)str_len(repl_text) - 1;
            count++;
        }
    }
    str_copy(m, "Replaced ", sizeof(m));
    str_cat(m, fmt_u64(num, (uint64_t)count, 10), sizeof(m));
    str_cat(m, " occurrence(s)", sizeof(m));
    set_msg(m);
}

/* ---- commands / palette ------------------------------------------------------ */

typedef struct {
    const char *name, *keys;
    int id;
} cmd_t;

enum {
    CMD_NEW, CMD_SAVE, CMD_SAVEAS, CMD_WSNEW, CMD_CLOSE, CMD_UNDO, CMD_REDO, CMD_FIND, CMD_REPLACE, CMD_GOTO,
    CMD_COMMENT, CMD_DELLINE, CMD_DUPLINE, CMD_MOVEUP, CMD_MOVEDOWN, CMD_SELALL, CMD_EMMET, CMD_SUGGEST,
    CMD_SIDEBAR, CMD_MINIMAP, CMD_LANG, CMD_INDENT, CMD_OUTDENT, CMD_REFRESH, CMD_QUICK
};

static const cmd_t cmds[] = {
    { "File: New File", "Ctrl+N", CMD_NEW }, { "File: Save", "Ctrl+S", CMD_SAVE },
    { "File: Save As (Home)...", "", CMD_SAVEAS }, { "Workspace: Save as Encrypted Workspace File...", "", CMD_WSNEW },
    { "View: Close Editor", "Ctrl+W", CMD_CLOSE }, { "Edit: Undo", "Ctrl+Z", CMD_UNDO },
    { "Edit: Redo", "Ctrl+Y", CMD_REDO }, { "Edit: Find", "Ctrl+F", CMD_FIND },
    { "Edit: Replace", "Ctrl+R", CMD_REPLACE }, { "Go to Line...", "Ctrl+G", CMD_GOTO },
    { "Toggle Line Comment", "Ctrl+/", CMD_COMMENT }, { "Delete Line", "Ctrl+Shift+K", CMD_DELLINE },
    { "Duplicate Line", "Shift+Alt+Down", CMD_DUPLINE }, { "Move Line Up", "Alt+Up", CMD_MOVEUP },
    { "Move Line Down", "Alt+Down", CMD_MOVEDOWN }, { "Select All", "Ctrl+A", CMD_SELALL },
    { "Emmet: Expand Abbreviation", "Tab", CMD_EMMET }, { "Trigger Suggest", "Ctrl+Space", CMD_SUGGEST },
    { "View: Toggle Side Bar", "Ctrl+B", CMD_SIDEBAR }, { "View: Toggle Minimap", "", CMD_MINIMAP },
    { "Change Language Mode", "", CMD_LANG }, { "Indent Line", "Tab", CMD_INDENT },
    { "Outdent Line", "Shift+Tab", CMD_OUTDENT }, { "Workspace: Refresh Files", "", CMD_REFRESH },
    { "Go to File...", "Ctrl+P", CMD_QUICK },
};
#define NCMDS ((int)(sizeof(cmds) / sizeof(cmds[0])))

static int ci_contains(const char *hay, const char *needle) {
    for (int i = 0; hay[i]; i++) {
        int k = 0;
        while (needle[k] && hay[i + k]) {
            char a = hay[i + k], b = needle[k];
            if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
            if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
            if (a != b) break;
            k++;
        }
        if (!needle[k]) return 1;
    }
    return !needle[0];
}

/* Items in the current palette: commands, files, or languages. */
static int pal_items(const char **names, const char **keys, int *ids, int cap) {
    int n = 0;
    if (overlay == OV_PALETTE) {
        for (int i = 0; i < NCMDS && n < cap; i++) {
            if (ci_contains(cmds[i].name, ov_text)) {
                names[n] = cmds[i].name;
                keys[n] = cmds[i].keys;
                ids[n++] = cmds[i].id;
            }
        }
    } else if (overlay == OV_QUICK) {
        for (int i = 0; i < nwfiles && n < cap; i++) {
            if (wfiles[i].open && ci_contains(wfiles[i].name, ov_text)) {
                names[n] = wfiles[i].name;
                keys[n] = "workspace (encrypted)";
                ids[n++] = i;
            }
        }
        for (int i = 0; i < nhfiles && n < cap; i++) {
            if (ci_contains(hfiles[i], ov_text)) {
                names[n] = hfiles[i];
                keys[n] = home;
                ids[n++] = 1000 + i;
            }
        }
    } else if (overlay == OV_LANG) {
        for (int i = 0; i < L_COUNT && n < cap; i++) {
            if (ci_contains(lang_name(i), ov_text)) {
                names[n] = lang_name(i);
                keys[n] = "";
                ids[n++] = i;
            }
        }
    }
    return n;
}

static void run_cmd(int id);

static void palette_accept(void) {
    const char *names[24], *keys[24];
    int ids[24], n = pal_items(names, keys, ids, 24), kind = overlay;
    overlay = OV_NONE;
    if (ov_sel >= n) return;
    if (kind == OV_PALETTE) run_cmd(ids[ov_sel]);
    else if (kind == OV_QUICK) {
        if (ids[ov_sel] >= 1000) {
            char p[160];
            str_copy(p, home, sizeof(p));
            str_cat(p, "/", sizeof(p));
            str_cat(p, hfiles[ids[ov_sel] - 1000], sizeof(p));
            open_home(p);
        } else {
            open_ws(ids[ov_sel]);
        }
    } else if (kind == OV_LANG && T()) {
        T()->lang = ids[ov_sel];
        reindex(T());
    }
}

static void open_overlay(int kind) {
    overlay = kind;
    ov_text[0] = '\0';
    ov_sel = 0;
    sugg_open = 0;
}

static void run_cmd(int id) {
    tab_t *t = T();
    switch (id) {
    case CMD_NEW: new_file(); return;
    case CMD_REFRESH: load_lists(); set_msg("Refreshed"); return;
    case CMD_SIDEBAR: show_side = !show_side; return;
    case CMD_MINIMAP: show_minimap = !show_minimap; return;
    case CMD_QUICK: load_lists(); open_overlay(OV_QUICK); return;
    }
    if (!t) return;
    switch (id) {
    case CMD_SAVE: save(); break;
    case CMD_SAVEAS: open_overlay(OV_SAVEAS); str_copy(ov_text, t->title, sizeof(ov_text)); break;
    case CMD_WSNEW: open_overlay(OV_WSNEW); str_copy(ov_text, t->title, sizeof(ov_text)); break;
    case CMD_CLOSE: close_tab(active); break;
    case CMD_UNDO: apply_undo(t, 0); break;
    case CMD_REDO: apply_undo(t, 1); break;
    case CMD_FIND: find_open = 1; find_focus_replace = 0; break;
    case CMD_REPLACE: find_open = 2; find_focus_replace = 1; break;
    case CMD_GOTO: open_overlay(OV_GOTO); break;
    case CMD_COMMENT: toggle_comment(t); break;
    case CMD_DELLINE: delete_line(t); break;
    case CMD_DUPLINE: duplicate_line(t); break;
    case CMD_MOVEUP: move_line(t, 0); break;
    case CMD_MOVEDOWN: move_line(t, 1); break;
    case CMD_SELALL: t->anchor = 0; t->cur = t->len; break;
    case CMD_EMMET: if (!try_emmet(t)) set_msg("No Emmet abbreviation before the cursor"); break;
    case CMD_SUGGEST: build_suggestions(1); break;
    case CMD_LANG: open_overlay(OV_LANG); break;
    case CMD_INDENT: indent_lines(t, 0); break;
    case CMD_OUTDENT: indent_lines(t, 1); break;
    }
    if (T()) scroll_to_cursor(T());
}

/* ---- drawing ------------------------------------------------------------------- */

static void add_hit(int x, int y, int w, int h, int a, int arg) {
    if (nhits < (int)(sizeof(hits) / sizeof(hits[0]))) {
        hits[nhits].r = (rect_t){ x, y, w, h };
        hits[nhits].action = a;
        hits[nhits].arg = arg;
        nhits++;
    }
}

static int hov(int x, int y, int w, int h) {
    return mouse_x >= x && mouse_y >= y && mouse_x < x + w && mouse_y < y + h;
}

static void draw_activity(surface_t *s) {
    draw_fill(s, 0, 0, ACT_W, win_h - STATUS_H, C_ACT);
    /* files: two pages */
    {
        uint32_t c = !side_search && show_side ? 0xffffff : 0x858585;
        if (!side_search && show_side) draw_fill(s, 0, 8, 2, 30, 0xffffff);
        draw_rrect_outline(s, 14, 12, 14, 18, 2, c, 255);
        draw_rrect_outline(s, 18, 16, 14, 18, 2, c, 255);
        add_hit(0, 6, ACT_W, 34, A_ACT_FILES, 0);
    }
    /* search: lens */
    {
        uint32_t c = side_search && show_side ? 0xffffff : 0x858585;
        if (side_search && show_side) draw_fill(s, 0, 48, 2, 30, 0xffffff);
        draw_rrect_outline(s, 13, 52, 14, 14, 7, c, 255);
        for (int k = 0; k < 6; k++) draw_fill(s, 25 + k, 64 + k, 2, 2, c);
        add_hit(0, 46, ACT_W, 34, A_ACT_SEARCH, 0);
    }
    /* the workspace, if any */
    if (ws_active) draw_icon(s, (ACT_W - 48) / 2 + 12, win_h - STATUS_H - 44, ICON_LOCK);
}

static void side_item(surface_t *s, int y, const char *name, int lock, int on, int action, int arg, uint32_t dot) {
    if (on || hov(ACT_W, y, SIDE_W, 22)) draw_fill(s, ACT_W, y, SIDE_W, 22, on ? 0x37373d : 0x2a2d2e);
    if (lock) draw_icon(s, ACT_W + 22, y + 3, ICON_LOCK);
    else draw_circle(s, ACT_W + 30, y + 11, 3, dot, 255);
    draw_text_clip(s, ACT_W + 44, y + 3, name, lock ? 0x8c8c8c : 0xcccccc, SIDE_W - 52);
    add_hit(ACT_W, y, SIDE_W, 22, action, arg);
}

static void draw_side(surface_t *s) {
    int y = 0;
    tab_t *t = T();
    draw_fill(s, ACT_W, 0, SIDE_W, win_h - STATUS_H, C_SIDE);
    if (side_search) {
        draw_text(s, ACT_W + 14, 10, "SEARCH", 0xbbbbbb);
        draw_round_fill(s, ACT_W + 10, 34, SIDE_W - 20, 26, 2, 0x3c3c3c);
        draw_text_clip(s, ACT_W + 16, 39, find_text[0] ? find_text : "Search (Ctrl+F)", find_text[0] ? 0xcccccc : 0x8c8c8c,
                       SIDE_W - 32);
        y = 70;
        if (t && find_text[0]) {
            int shown = 0;
            for (int i = 0; i < t->len && y < win_h - STATUS_H - 24 && shown < 40; i++) {
                if (match_at(t, i)) {
                    int l = line_of(i), e = line_end(t, l), n = e - ls[l];
                    char row[80], num[12];
                    if (n > 60) n = 60;
                    str_copy(row, fmt_u64(num, (uint64_t)l + 1, 10), sizeof(row));
                    str_cat(row, ": ", sizeof(row));
                    {
                        int k = (int)str_len(row), j = ls[l];
                        while (j < ls[l] + n && (t->buf[j] == ' ' || t->buf[j] == '\t')) j++;
                        while (j < ls[l] + n && k < 78) row[k++] = t->buf[j++];
                        row[k] = '\0';
                    }
                    side_item(s, y, row, 0, 0, A_SEARCH_HIT, i, 0xd7ba7d);
                    y += 22;
                    shown++;
                    i = e;
                }
            }
        }
        return;
    }
    draw_text(s, ACT_W + 14, 10, "EXPLORER", 0xbbbbbb);
    y = 34;
    if (ws_active) {
        char h[64];
        str_copy(h, "WORKSPACE: ", sizeof(h));
        str_cat(h, ws_name, sizeof(h));
        draw_fill(s, ACT_W, y, SIDE_W, 22, 0x2a2532);
        draw_text_clip_f(s, ACT_W + 10, y + 3, h, 0xd7b3ff, SIDE_W - 50, FONT_BOLD);
        draw_text(s, ACT_W + SIDE_W - 26, y + 3, "+", hov(ACT_W + SIDE_W - 30, y, 22, 22) ? 0xffffff : 0xaaaaaa);
        add_hit(ACT_W + SIDE_W - 30, y, 22, 22, A_SIDE_NEW, 0);
        y += 24;
        for (int i = 0; i < nwfiles && y < win_h - 140; i++) {
            int on = t && str_eq(t->ws_id, wfiles[i].id);
            side_item(s, y, wfiles[i].open ? wfiles[i].name : "locked file", !wfiles[i].open, on, A_WSFILE, i,
                      lang_col[lang_detect(wfiles[i].name, "", 0)]);
            y += 22;
        }
        if (!nwfiles) {
            draw_text(s, ACT_W + 22, y + 2, "no shared files yet", 0x6e6e6e);
            y += 22;
        }
        y += 6;
    }
    draw_fill(s, ACT_W, y, SIDE_W, 22, 0x2d2d30);
    draw_text_clip_f(s, ACT_W + 10, y + 3, "HOME", 0xcccccc, SIDE_W - 50, FONT_BOLD);
    draw_text(s, ACT_W + SIDE_W - 26, y + 3, "~", hov(ACT_W + SIDE_W - 30, y, 22, 22) ? 0xffffff : 0xaaaaaa);
    add_hit(ACT_W + SIDE_W - 30, y, 22, 22, A_SIDE_REFRESH, 0);
    y += 24;
    for (int i = 0; i < nhfiles && y < win_h - STATUS_H - 24; i++) {
        char p[160];
        int on;
        str_copy(p, home, sizeof(p));
        str_cat(p, "/", sizeof(p));
        str_cat(p, hfiles[i], sizeof(p));
        on = t && str_eq(t->path, p);
        side_item(s, y, hfiles[i], 0, on, A_HOMEFILE, i, lang_col[lang_detect(hfiles[i], "", 0)]);
        y += 22;
    }
}

static void draw_tabs(surface_t *s) {
    int x = ed_x();
    draw_fill(s, x, 0, win_w - x, TAB_H, C_TABBAR);
    for (int i = 0; i < MAX_TABS; i++) {
        tab_t *t = &tabs[i];
        int w, on = i == active;
        if (!t->used) continue;
        w = text_width(t->title) + 62;
        if (w > 200) w = 200;
        draw_fill(s, x, 0, w, TAB_H, on ? C_BG : C_TAB);
        if (on) draw_fill(s, x, 0, w, 1, t->ws_id[0] ? 0xb180d7 : C_STATUS);
        draw_fill(s, x + w - 1, 0, 1, TAB_H, 0x252526);
        if (t->ws_id[0]) draw_icon(s, x + 10, 9, ICON_LOCK);
        else draw_circle(s, x + 17, TAB_H / 2, 4, lang_col[t->lang], 255);
        draw_text_clip(s, x + 30, 9, t->title, on ? 0xffffff : 0x969696, w - 58);
        if (t->dirty && !hov(x + w - 26, 7, 20, 20)) {
            draw_circle(s, x + w - 16, TAB_H / 2, 4, on ? 0xffffff : 0x969696, 255);
        } else if (on || hov(x, 0, w, TAB_H)) {
            int cx = x + w - 16, cy = TAB_H / 2;
            uint32_t c = hov(x + w - 26, 7, 20, 20) ? 0xffffff : 0xa0a0a0;
            for (int k = -4; k <= 4; k++) {
                draw_fill(s, cx + k, cy + k, 1, 1, c);
                draw_fill(s, cx + k, cy - k, 1, 1, c);
            }
        }
        add_hit(x + w - 26, 7, 20, 20, A_TAB_CLOSE, i);
        add_hit(x, 0, w, TAB_H, A_TAB, i);
        x += w;
    }
}

static void draw_crumbs(surface_t *s, tab_t *t) {
    int x = ed_x() + 14, y = TAB_H + 3;
    char c[200];
    draw_fill(s, ed_x(), TAB_H, win_w - ed_x(), CRUMB_H, C_BG);
    if (!t) return;
    if (t->ws_id[0]) {
        str_copy(c, "Workspace  >  ", sizeof(c));
        str_cat(c, ws_name, sizeof(c));
        str_cat(c, "  >  ", sizeof(c));
        str_cat(c, t->title, sizeof(c));
    } else {
        str_copy(c, t->path[0] ? t->path : t->title, sizeof(c));
        for (int i = 0; c[i]; i++) {
            if (c[i] == '/') c[i] = '>';
        }
    }
    draw_text_clip(s, x, y, c, 0xa9a9a9, win_w - x - 220);
    if (t->ws_id[0]) {
        const char *b = t->readonly ? "read-only  -  encrypted" : "end-to-end encrypted";
        int bw = text_width(b);
        draw_icon(s, win_w - bw - 40, y, ICON_LOCK);
        draw_text(s, win_w - bw - 20, y, b, t->readonly ? 0xe6b450 : 0xb180d7);
    }
}

static int bracket_match(tab_t *t, int *a, int *b) {
    static const char open[] = "([{", close[] = ")]}";
    int p = t->cur;
    for (int side = 0; side < 2; side++) {
        int i = side ? p - 1 : p;
        if (i < 0 || i >= t->len) continue;
        for (int k = 0; k < 3; k++) {
            if (t->buf[i] == open[k]) {
                int depth = 0;
                for (int j = i; j < t->len; j++) {
                    if (t->buf[j] == open[k]) depth++;
                    else if (t->buf[j] == close[k] && --depth == 0) {
                        *a = i;
                        *b = j;
                        return 1;
                    }
                }
            } else if (t->buf[i] == close[k]) {
                int depth = 0;
                for (int j = i; j >= 0; j--) {
                    if (t->buf[j] == close[k]) depth++;
                    else if (t->buf[j] == open[k] && --depth == 0) {
                        *a = j;
                        *b = i;
                        return 1;
                    }
                }
            }
        }
    }
    return 0;
}

static void draw_editor(surface_t *s, tab_t *t) {
    int x0 = ed_x(), y0 = ed_y(), tx = text_x(), tw = text_w(), nrows = rows();
    int cl = line_of(t->cur), sa, sb, has_sel = sel_range(t, &sa, &sb), ba = -1, bb = -1, fl = (int)str_len(find_text);
    draw_fill(s, x0, y0, win_w - x0, ed_h(), C_BG);
    if (!has_sel) (void)bracket_match(t, &ba, &bb);
    for (int r = 0; r <= nrows && t->top + r < nlines; r++) {
        int l = t->top + r, y = y0 + r * LH, st = ls[l], en = line_end(t, l), n = en - st, col = 0, guide = 0;
        char num[12];
        if (l == cl && !has_sel) {
            draw_fill(s, x0, y, win_w - x0 - (show_minimap ? MINIMAP : 0) - 12, LH, C_BG);
            draw_frame(s, tx - 4, y, tw + 4, LH, C_LINE);
        }
        fmt_u64(num, (uint64_t)l + 1, 10);
        draw_text_f(s, tx - 18 - text_width_f(num, FONT_MONO), y + 1, num, l == cl ? C_GUTTER_HI : C_GUTTER, FONT_MONO);
        if (n > (int)sizeof(cls)) n = (int)sizeof(cls);
        lang_line(t->lang, t->buf + st, n, lstate[l], cls);
        for (int i = 0; i < n; i++) {
            char c = t->buf[st + i];
            int w = c == '\t' ? (col / TABSTOP + 1) * TABSTOP - col : 1, px = tx + (col - t->left) * CW;
            int pos = st + i;
            if (!guide && c != ' ' && c != '\t') guide = col;
            if (col + w > t->left && px < tx + tw) {
                if (has_sel && pos >= sa && pos < sb) draw_fill(s, px, y, w * CW, LH, C_SEL);
                else if (find_open && fl && match_at(t, pos)) draw_fill(s, px, y, fl * CW, LH, C_FIND);
                if (pos == ba || pos == bb) draw_frame(s, px, y, CW, LH, 0x888888);
                if (c != ' ' && c != '\t' && col >= t->left) {
                    uint32_t fg = tok_col[cls[i] < T_COUNT ? cls[i] : 0];
                    draw_char(s, px, y + 1, c, fg);
                }
            }
            col += w;
        }
        if (!guide && n) guide = col;
        /* indentation guides */
        for (int g = TABSTOP; g < guide; g += TABSTOP) {
            int gx = tx + (g - t->left) * CW;
            if (gx > tx) draw_fill(s, gx, y, 1, LH, l == cl ? 0x707070 : 0x404040);
        }
        if (has_sel && sa <= en && sb > en && en < t->len) {
            int px = tx + (col - t->left) * CW;
            if (px >= tx) draw_fill(s, px, y, CW, LH, C_SEL);
        }
    }
    /* cursor */
    {
        int r = cl - t->top, c = vcol(t, t->cur) - t->left;
        if (r >= 0 && r <= nrows && c >= 0 && overlay == OV_NONE) draw_fill(s, tx + c * CW, y0 + r * LH + 1, 2, LH - 2, 0xaeafad);
    }
    add_hit(x0, y0, tx + tw - x0, ed_h(), A_TEXT, 0);

    /* minimap */
    if (show_minimap) {
        int mx = win_w - MINIMAP - 12, mh = ed_h(), scale = nlines * 2 > mh ? (nlines * 2 + mh - 1) / mh : 1;
        draw_fill(s, mx, y0, MINIMAP, mh, 0x1e1e1e);
        draw_fill(s, mx, y0, 1, mh, 0x2b2b2b);
        for (int l = 0; l < nlines && (l / scale) * 2 < mh; l += scale) {
            int st = ls[l], en = line_end(t, l), n = en - st, col = 0, y = y0 + (l / scale) * 2;
            if (n > 80) n = 80;
            lang_line(t->lang, t->buf + st, n, lstate[l], cls);
            for (int i = 0; i < n && col < 80; i++) {
                char c = t->buf[st + i];
                if (c == '\t') {
                    col += TABSTOP;
                    continue;
                }
                if (c != ' ') draw_fill_a(s, mx + 6 + col, y, 1, 2, tok_col[cls[i] < T_COUNT ? cls[i] : 0], 150);
                col++;
            }
        }
        {
            int vy = y0 + (t->top / scale) * 2, vh = (nrows / scale) * 2;
            if (vh < 8) vh = 8;
            draw_fill_a(s, mx + 1, vy, MINIMAP - 1, vh, 0xffffff, hov(mx, vy, MINIMAP, vh) ? 28 : 16);
        }
        add_hit(mx, y0, MINIMAP, mh, A_MINIMAP, scale);
    }
    /* scrollbar */
    {
        int sx = win_w - 12, sh = ed_h();
        draw_fill(s, sx, y0, 12, sh, C_BG);
        if (nlines > nrows) {
            int th = sh * nrows / nlines, ty = y0 + sh * t->top / nlines;
            if (th < 20) th = 20;
            draw_fill_a(s, sx + 2, ty, 9, th, 0x797979, 100);
        }
    }
}

static void draw_status(surface_t *s, tab_t *t) {
    int y = win_h - STATUS_H, x;
    char b[96], n[16];
    draw_fill(s, 0, y, win_w, STATUS_H, C_STATUS);
    /* left: where the file lives */
    if (t && t->ws_id[0]) {
        str_copy(b, "  Workspace: ", sizeof(b));
        str_cat(b, ws_name, sizeof(b));
        str_cat(b, "  ", sizeof(b));
        draw_fill(s, 0, y, text_width(b) + 22, STATUS_H, C_WSBAR);
        draw_icon(s, 6, y + 3, ICON_LOCK);
        draw_text(s, 14, y + 3, b, 0xffffff);
        x = text_width(b) + 34;
    } else {
        draw_fill(s, 0, y, 70, STATUS_H, 0x16825d);
        draw_text(s, 12, y + 3, "Home", 0xffffff);
        x = 84;
    }
    if (message[0] && sh_ticks() - message_t < 800) draw_text_clip(s, x, y + 3, message, 0xffffff, win_w - x - 470);
    if (!t) return;
    /* right: position, indentation, encoding, language */
    x = win_w - 12;
    {
        const char *items[7];
        char pos[40];
        int k = 0;
        str_copy(pos, "Ln ", sizeof(pos));
        str_cat(pos, fmt_u64(n, (uint64_t)line_of(t->cur) + 1, 10), sizeof(pos));
        str_cat(pos, ", Col ", sizeof(pos));
        str_cat(pos, fmt_u64(n, (uint64_t)vcol(t, t->cur) + 1, 10), sizeof(pos));
        if (lang_emmet(t->lang)) items[k++] = "Emmet";
        items[k++] = lang_name(t->lang);
        items[k++] = "LF";
        items[k++] = "UTF-8";
        items[k++] = "Spaces: 4";
        items[k++] = pos;
        if (t->readonly) items[k++] = "Read-only";
        for (int i = 0; i < k; i++) {
            int w = text_width(items[i]);
            x -= w;
            if (items[i] == lang_name(t->lang)) {
                if (hov(x - 6, y, w + 12, STATUS_H)) draw_fill(s, x - 6, y, w + 12, STATUS_H, 0x1f8ad2);
                add_hit(x - 6, y, w + 12, STATUS_H, A_STATUS_LANG, 0);
            }
            draw_text(s, x, y + 3, items[i], 0xffffff);
            x -= 22;
        }
    }
}

static void widget_box(surface_t *s, int x, int y, int w, int h) {
    draw_shadow(s, x, y, w, h, 3, 10, 0x000000, 140);
    draw_fill(s, x, y, w, h, C_WIDGET);
    draw_frame(s, x, y, w, h, C_WIDGET_EDGE);
}

static void draw_input(surface_t *s, int x, int y, int w, const char *text, const char *ph, int focused) {
    draw_fill(s, x, y, w, 26, 0x3c3c3c);
    if (focused) draw_frame(s, x, y, w, 26, 0x007fd4);
    draw_text_clip(s, x + 8, y + 5, text[0] ? text : ph, text[0] ? 0xcccccc : 0x8c8c8c, w - 16);
    if (focused) {
        int cx = x + 8 + text_width(text);
        draw_fill(s, cx < x + w - 4 ? cx : x + w - 4, y + 5, 1, 16, 0xcccccc);
    }
}

static void draw_find(surface_t *s, tab_t *t) {
    int w = 380, h = find_open == 2 ? 66 : 36, x = win_w - w - MINIMAP - 24, y = ed_y();
    char cnt[32], n[12];
    int idx, total;
    widget_box(s, x, y, w, h);
    draw_input(s, x + 8, y + 5, 210, find_text, "Find", !find_focus_replace);
    total = count_matches(t, &idx);
    if (!find_text[0]) str_copy(cnt, "No results", sizeof(cnt));
    else if (!total) str_copy(cnt, "No results", sizeof(cnt));
    else {
        str_copy(cnt, fmt_u64(n, (uint64_t)(idx ? idx : 1), 10), sizeof(cnt));
        str_cat(cnt, " of ", sizeof(cnt));
        str_cat(cnt, fmt_u64(n, (uint64_t)total, 10), sizeof(cnt));
    }
    draw_text(s, x + 226, y + 10, cnt, total || !find_text[0] ? 0xcccccc : 0xf48771);
    draw_text(s, x + 306, y + 9, "^", hov(x + 300, y + 5, 20, 26) ? 0xffffff : 0xaaaaaa);
    draw_text(s, x + 326, y + 9, "v", hov(x + 320, y + 5, 20, 26) ? 0xffffff : 0xaaaaaa);
    draw_text(s, x + 352, y + 9, "x", hov(x + 346, y + 5, 20, 26) ? 0xffffff : 0xaaaaaa);
    add_hit(x + 300, y + 5, 20, 26, A_FIND_PREV, 0);
    add_hit(x + 320, y + 5, 20, 26, A_FIND_NEXT, 0);
    add_hit(x + 346, y + 5, 20, 26, A_FIND_CLOSE, 0);
    if (find_open == 2) {
        draw_input(s, x + 8, y + 35, 210, repl_text, "Replace", find_focus_replace);
        ui_button(s, x + 226, y + 35, 60, 26, "All", 0, hov(x + 226, y + 35, 60, 26));
        add_hit(x + 226, y + 35, 60, 26, A_REPLACE_ALL, 0);
    }
}

static void draw_suggest(surface_t *s, tab_t *t) {
    int r = line_of(t->cur) - t->top, cx = text_x() + (vcol(t, sugg_from) - t->left) * CW;
    int w = 420, rowh = 22, h = nsugg * rowh + 2, y = ed_y() + (r + 1) * LH, prev_h = 0;
    static const char *const kinds[] = { "Emmet Abbreviation", "snippet", "keyword", "word" };
    static const uint32_t kc[] = { 0x89d185, 0xd7ba7d, 0x569cd6, 0x9cdcfe };
    if (cx + w > win_w - 20) cx = win_w - 20 - w;
    if (y + h > win_h - STATUS_H - 4) y = ed_y() + r * LH - h;
    widget_box(s, cx, y, w, h);
    for (int i = 0; i < nsugg; i++) {
        int yy = y + 1 + i * rowh;
        if (i == sugg_sel) draw_fill(s, cx + 1, yy, w - 2, rowh, 0x04395e);
        draw_fill(s, cx + 8, yy + 6, 10, 10, kc[sugg[i].kind]);
        draw_text_clip_f(s, cx + 26, yy + 3, sugg[i].label, i == sugg_sel ? 0xffffff : 0xcccccc, 220, FONT_MONO);
        draw_text(s, cx + w - 8 - text_width(kinds[sugg[i].kind]), yy + 3, kinds[sugg[i].kind], 0x8c8c8c);
        add_hit(cx, yy, w, rowh, A_SUGG, i);
    }
    /* details: what the Emmet abbreviation or snippet turns into */
    if (nsugg && (sugg[sugg_sel].kind == S_EMMET || sugg[sugg_sel].kind == S_SNIPPET)) {
        const char *p = sugg[sugg_sel].insert;
        int lines = 0, py;
        for (const char *q = p; *q && lines < 10; q++) lines += *q == '\n';
        prev_h = (lines + 1 < 10 ? lines + 1 : 10) * 16 + 12;
        py = y + h + 2;
        if (py + prev_h > win_h - STATUS_H) py = y - prev_h - 2;
        widget_box(s, cx, py, w, prev_h);
        for (int k = 0; *p && k < 10; k++) {
            char row[120];
            int j = 0;
            /* later lines carry the current line's indentation: drop it */
            if (k) {
                char ind[64];
                int il = indent_of(t, line_of(t->cur), ind, sizeof(ind));
                for (int q = 0; q < il && (*p == ' ' || *p == '\t'); q++) p++;
            }
            while (*p && *p != '\n' && j < 110) {
                if (*p != EMMET_CURSOR) row[j++] = *p;
                p++;
            }
            row[j] = '\0';
            if (*p == '\n') p++;
            draw_text_clip_f(s, cx + 10, py + 6 + k * 16, row, 0xd4d4d4, w - 20, FONT_MONO);
        }
    }
}

static void draw_palette(surface_t *s) {
    const char *names[24], *keys[24];
    int ids[24], n = pal_items(names, keys, ids, 24), w = 560, x = ed_x() + (win_w - ed_x() - w) / 2, y = 8, h;
    const char *ph = overlay == OV_PALETTE ? "Type a command" : overlay == OV_QUICK ? "Search files by name"
                   : overlay == OV_LANG ? "Select a language" : overlay == OV_GOTO ? "Type a line number to go to"
                   : overlay == OV_SAVEAS ? "File name (saved in your home, private)"
                   : "Name for the new encrypted workspace file";
    int list = overlay == OV_PALETTE || overlay == OV_QUICK || overlay == OV_LANG;
    if (n > 12) n = 12;
    h = 40 + (list ? n * 24 + 4 : 24);
    widget_box(s, x, y, w, h);
    draw_input(s, x + 8, y + 8, w - 16, ov_text, ph, 1);
    if (overlay == OV_PALETTE && !ov_text[0]) draw_text(s, x + w - 24, y + 13, ">", 0x8c8c8c);
    if (!list) {
        const char *hint = overlay == OV_GOTO ? "Press Enter to go to the line" : "Press Enter to save, Esc to cancel";
        draw_text(s, x + 12, y + 40, hint, 0x8c8c8c);
        return;
    }
    for (int i = 0; i < n; i++) {
        int yy = y + 40 + i * 24;
        if (i == ov_sel) draw_fill(s, x + 1, yy, w - 2, 24, 0x04395e);
        draw_text_clip(s, x + 12, yy + 4, names[i], 0xcccccc, w - 220);
        if (keys[i] && keys[i][0]) draw_text_clip(s, x + w - 12 - text_width(keys[i]), yy + 4, keys[i], 0x8c8c8c, 200);
        add_hit(x, yy, w, 24, A_PALETTE_ITEM, i);
    }
}

static void draw_welcome(surface_t *s) {
    int x0 = ed_x(), y0 = ed_y(), cx = x0 + (win_w - x0) / 2;
    static const char *const lines[][2] = {
        { "Show All Commands", "F1 / Ctrl+Shift+P" }, { "Go to File", "Ctrl+P" }, { "New File", "Ctrl+N" },
        { "Find", "Ctrl+F" }, { "Toggle Side Bar", "Ctrl+B" },
    };
    draw_fill(s, x0, y0 - CRUMB_H, win_w - x0, ed_h() + CRUMB_H, C_BG);
    draw_icon(s, cx - 24, y0 + 70, ICON_CODE);
    draw_text_f(s, cx - text_width_f("Code", FONT_BOLD) / 2, y0 + 130, "Code", 0xcccccc, FONT_BOLD);
    draw_text(s, cx - text_width("Open a file from the Explorer, or start a new one.") / 2, y0 + 152,
              "Open a file from the Explorer, or start a new one.", 0x8c8c8c);
    for (int i = 0; i < 5; i++) {
        int y = y0 + 196 + i * 26;
        draw_text(s, cx - 200, y, lines[i][0], 0x9d9d9d);
        draw_text_f(s, cx + 40, y, lines[i][1], 0xcccccc, FONT_MONO);
    }
}

static void render(void) {
    surface_t *s = ui_surface();
    tab_t *t = T();
    nhits = 0;
    draw_fill(s, 0, 0, win_w, win_h, C_BG);
    if (t) reindex(t);
    /* order matters for hit testing: overlays are added last and win */
    if (t) draw_editor(s, t);
    else draw_welcome(s);
    draw_tabs(s);
    draw_crumbs(s, t);
    draw_activity(s);
    if (show_side) draw_side(s);
    draw_status(s, t);
    if (t && find_open) draw_find(s, t);
    if (t && sugg_open && overlay == OV_NONE) draw_suggest(s, t);
    if (overlay != OV_NONE) draw_palette(s);
    ui_present();
}

/* ---- input --------------------------------------------------------------------- */

enum {
    K_NONE, K_CHAR, K_UP, K_DOWN, K_LEFT, K_RIGHT, K_HOME, K_END, K_PGUP, K_PGDN, K_DEL, K_INS, K_ESC, K_BACKTAB,
    K_F1, K_F2, K_F3, K_F5, K_CTRL /* + letter in ch */, K_CSIU /* code in ch */
};
#define M_SHIFT 1
#define M_ALT 2
#define M_CTRL 4

static int esc_state, esc_n, esc_p[4];
static uint64_t esc_t;

static void handle_key(int k, int ch, int mods);

/* Bytes from the window manager -> keys, decoding xterm sequences. */
static void key_byte(uint32_t b) {
    if (esc_state == 1) {
        if (b == '[' || b == 'O') {
            esc_state = b == '[' ? 2 : 3;
            esc_n = 0;
            esc_p[0] = esc_p[1] = 0;
            return;
        }
        esc_state = 0;
        handle_key(K_ESC, 0, 0);
    }
    if (esc_state == 3) { /* ESC O P..S */
        esc_state = 0;
        if (b >= 'P' && b <= 'S') handle_key(K_F1 + (int)(b - 'P') > K_F3 ? K_F3 : K_F1 + (int)(b - 'P'), 0, 0);
        return;
    }
    if (esc_state == 2) {
        int m;
        if (b >= '0' && b <= '9') {
            esc_p[esc_n] = esc_p[esc_n] * 10 + (int)(b - '0');
            return;
        }
        if (b == ';') {
            if (esc_n < 3) esc_n++;
            esc_p[esc_n] = 0;
            return;
        }
        esc_state = 0;
        m = esc_n ? esc_p[esc_n] - 1 : 0;
        switch (b) {
        case 'A': handle_key(K_UP, 0, m); return;
        case 'B': handle_key(K_DOWN, 0, m); return;
        case 'C': handle_key(K_RIGHT, 0, m); return;
        case 'D': handle_key(K_LEFT, 0, m); return;
        case 'H': handle_key(K_HOME, 0, m); return;
        case 'F': handle_key(K_END, 0, m); return;
        case 'P': handle_key(K_F1, 0, m); return;
        case 'R': handle_key(K_F3, 0, m); return;
        case 'Z': handle_key(K_BACKTAB, 0, M_SHIFT); return;
        case 'u': handle_key(K_CSIU, esc_p[0], m); return;
        case '~':
            switch (esc_p[0]) {
            case 2: handle_key(K_INS, 0, m); return;
            case 3: handle_key(K_DEL, 0, m); return;
            case 5: handle_key(K_PGUP, 0, m); return;
            case 6: handle_key(K_PGDN, 0, m); return;
            case 15: handle_key(K_F5, 0, m); return;
            }
            return;
        }
        return;
    }
    if (b == 27) {
        esc_state = 1;
        esc_t = sh_ticks();
        return;
    }
    if (b == '\n' || b == '\r') handle_key(K_CHAR, '\n', 0);
    else if (b == '\t') handle_key(K_CHAR, '\t', 0);
    else if (b == '\b' || b == 127) handle_key(K_CHAR, '\b', 0);
    else if (b >= 1 && b <= 26) handle_key(K_CTRL, (int)('a' + b - 1), M_CTRL);
    else if (b >= 32 && b < 127) handle_key(K_CHAR, (int)b, 0);
}

static void move_cursor(tab_t *t, int k, int mods) {
    int l = line_of(t->cur), shift = mods & M_SHIFT, ctrl = mods & M_CTRL, a, b;
    int has = sel_range(t, &a, &b);
    if (t->want_col < 0) t->want_col = vcol(t, t->cur);
    switch (k) {
    case K_LEFT:
        if (has && !shift) t->cur = a;
        else if (ctrl) {
            int p = t->cur;
            while (p > 0 && !is_word(t->buf[p - 1])) p--;
            while (p > 0 && is_word(t->buf[p - 1])) p--;
            t->cur = p;
        } else if (t->cur > 0) t->cur--;
        t->want_col = -1;
        break;
    case K_RIGHT:
        if (has && !shift) t->cur = b;
        else if (ctrl) {
            int p = t->cur;
            while (p < t->len && !is_word(t->buf[p])) p++;
            while (p < t->len && is_word(t->buf[p])) p++;
            t->cur = p;
        } else if (t->cur < t->len) t->cur++;
        t->want_col = -1;
        break;
    case K_UP:
    case K_DOWN:
    case K_PGUP:
    case K_PGDN: {
        int step = k == K_UP ? -1 : k == K_DOWN ? 1 : k == K_PGUP ? -(rows() - 1) : rows() - 1, nl = l + step;
        int wc = t->want_col;
        if (nl < 0) nl = 0;
        if (nl >= nlines) nl = nlines - 1;
        t->cur = pos_at_vcol(t, nl, wc);
        t->want_col = wc;
        if (k == K_PGUP || k == K_PGDN) t->top += step;
        break;
    }
    case K_HOME:
        if (ctrl) t->cur = 0;
        else {
            int s = ls[l], f = s;
            while (f < line_end(t, l) && (t->buf[f] == ' ' || t->buf[f] == '\t')) f++;
            t->cur = t->cur == f ? s : f; /* smart Home */
        }
        t->want_col = -1;
        break;
    case K_END:
        t->cur = ctrl ? t->len : line_end(t, l);
        t->want_col = -1;
        break;
    }
    if (!shift) t->anchor = t->cur;
    if (t->top < 0) t->top = 0;
    scroll_to_cursor(t);
}

static void overlay_key(int k, int ch) {
    int len = (int)str_len(ov_text);
    if (k == K_ESC) {
        overlay = OV_NONE;
        return;
    }
    if (k == K_UP && ov_sel > 0) ov_sel--;
    if (k == K_DOWN) ov_sel++;
    if (k == K_CHAR && ch == '\n') {
        if (overlay == OV_GOTO) {
            int n = 0;
            for (char *p = ov_text; *p >= '0' && *p <= '9'; p++) n = n * 10 + (*p - '0');
            overlay = OV_NONE;
            if (T() && n > 0) {
                T()->cur = T()->anchor = ls[n - 1 < nlines ? n - 1 : nlines - 1];
                scroll_to_cursor(T());
            }
        } else if (overlay == OV_SAVEAS) {
            overlay = OV_NONE;
            save_as(ov_text);
        } else if (overlay == OV_WSNEW) {
            overlay = OV_NONE;
            save_to_workspace(ov_text);
        } else {
            palette_accept();
        }
        return;
    }
    if (k == K_CHAR && ch == '\b') {
        if (len) ov_text[len - 1] = '\0';
        ov_sel = 0;
    } else if (k == K_CHAR && ch >= 32 && len < (int)sizeof(ov_text) - 1) {
        ov_text[len] = (char)ch;
        ov_text[len + 1] = '\0';
        ov_sel = 0;
    }
}

static void find_key(tab_t *t, int k, int ch, int mods) {
    char *f = find_focus_replace ? repl_text : find_text;
    int len = (int)str_len(f);
    if (k == K_ESC) {
        find_open = 0;
        return;
    }
    if (k == K_CHAR && ch == '\n') {
        if (find_focus_replace) replace_one(t);
        else find_step(t, 0);
        return;
    }
    if (k == K_CHAR && ch == '\t' && find_open == 2) {
        find_focus_replace = !find_focus_replace;
        return;
    }
    if (k == K_F3) {
        find_step(t, mods & M_SHIFT);
        return;
    }
    if (k == K_CHAR && ch == '\b') {
        if (len) f[len - 1] = '\0';
    } else if (k == K_CHAR && ch >= 32 && len < 95) {
        f[len] = (char)ch;
        f[len + 1] = '\0';
        if (!find_focus_replace) {
            /* search as you type, from where the selection starts */
            int a, b;
            sel_range(t, &a, &b);
            t->cur = t->anchor = a;
            for (int i = a; i < t->len; i++) {
                if (match_at(t, i)) {
                    t->anchor = i;
                    t->cur = i + (int)str_len(find_text);
                    scroll_to_cursor(t);
                    break;
                }
            }
        }
    }
}

static void handle_key(int k, int ch, int mods) {
    tab_t *t = T();
    int ctrl_shift = (mods & (M_CTRL | M_SHIFT)) == (M_CTRL | M_SHIFT);
    if (k != K_CTRL || ch != 'w') close_armed = -1;
    /* global shortcuts */
    if (k == K_F1 || (k == K_CSIU && ch == 'p' && ctrl_shift)) {
        open_overlay(OV_PALETTE);
        return;
    }
    if (overlay != OV_NONE) {
        overlay_key(k, ch);
        return;
    }
    if (k == K_CTRL) {
        switch (ch) {
        case 'n': run_cmd(CMD_NEW); return;
        case 'p': run_cmd(CMD_QUICK); return;
        case 'b': run_cmd(CMD_SIDEBAR); return;
        case 'w': run_cmd(CMD_CLOSE); return;
        case 'o': run_cmd(CMD_QUICK); return;
        }
    }
    if (!t) return;
    if (find_open && (k == K_CHAR || k == K_ESC || k == K_F3)) {
        find_key(t, k, ch, mods);
        return;
    }
    if (sugg_open) {
        if (k == K_UP) {
            sugg_sel = sugg_sel ? sugg_sel - 1 : nsugg - 1;
            return;
        }
        if (k == K_DOWN) {
            sugg_sel = (sugg_sel + 1) % nsugg;
            return;
        }
        if (k == K_CHAR && (ch == '\n' || ch == '\t')) {
            accept_suggestion();
            scroll_to_cursor(t);
            return;
        }
        if (k == K_ESC) {
            sugg_open = 0;
            return;
        }
    }
    if (k == K_ESC) {
        find_open = 0;
        t->anchor = t->cur;
        return;
    }
    if (k == K_CTRL) {
        switch (ch) {
        case 's': run_cmd(CMD_SAVE); break;
        case 'z': run_cmd(CMD_UNDO); break;
        case 'y': run_cmd(CMD_REDO); break;
        case 'f': run_cmd(CMD_FIND); break;
        case 'r': run_cmd(CMD_REPLACE); break;
        case 'g': run_cmd(CMD_GOTO); break;
        case 'a': run_cmd(CMD_SELALL); break;
        case 'c': copy_sel(t, 0); break;
        case 'x': copy_sel(t, 1); break;
        case 'v': paste(t); break;
        case 'd': select_word(t); break;
        case 'l': {
            int l = line_of(t->cur);
            t->anchor = ls[l];
            t->cur = l + 1 < nlines ? ls[l + 1] : t->len;
            break;
        }
        case 'k': run_cmd(CMD_DELLINE); break;
        case 'e': run_cmd(CMD_EMMET); break;
        }
        sugg_open = 0;
        scroll_to_cursor(t);
        return;
    }
    if (k == K_CSIU) {
        if (ch == ' ' && (mods & M_CTRL)) run_cmd(CMD_SUGGEST);
        else if (ch == '/' && (mods & M_CTRL)) run_cmd(CMD_COMMENT);
        else if (ch == 'k' && ctrl_shift) run_cmd(CMD_DELLINE);
        else if (ch == 'z' && ctrl_shift) run_cmd(CMD_REDO);
        else if (ch == 'd' && ctrl_shift) run_cmd(CMD_DUPLINE);
        else if (ch == 'f' && ctrl_shift) { show_side = 1; side_search = 1; }
        else if (ch == 'e' && ctrl_shift) { show_side = 1; side_search = 0; }
        scroll_to_cursor(t);
        return;
    }
    if ((k == K_UP || k == K_DOWN) && (mods & M_ALT)) {
        if (mods & M_SHIFT) duplicate_line(t);
        else move_line(t, k == K_DOWN);
        scroll_to_cursor(t);
        return;
    }
    if ((k == K_PGUP || k == K_PGDN) && (mods & M_CTRL)) {
        for (int i = 1; i <= MAX_TABS; i++) {
            int j = (active + (k == K_PGDN ? i : MAX_TABS - i)) % MAX_TABS;
            if (tabs[j].used) {
                active = j;
                break;
            }
        }
        return;
    }
    if (k == K_F3) {
        find_step(t, mods & M_SHIFT);
        return;
    }
    if (k >= K_UP && k <= K_PGDN) {
        sugg_open = 0;
        move_cursor(t, k, mods);
        return;
    }
    if (k == K_DEL) {
        int a, b;
        if (sel_range(t, &a, &b)) replace(t, a, b - a, "", 0, a);
        else if (t->cur < t->len) replace(t, t->cur, 1, "", 0, t->cur);
        return;
    }
    if (k == K_BACKTAB) {
        indent_lines(t, 1);
        return;
    }
    if (k != K_CHAR) return;
    if (ch == '\n') {
        sugg_open = 0;
        enter(t);
    } else if (ch == '\t') {
        int l0, l1;
        if (line_has_sel_lines(t, &l0, &l1)) indent_lines(t, 0);
        else if (!try_emmet(t)) {
            int c = vcol(t, t->cur), n = TABSTOP - c % TABSTOP;
            insert_text(t, "    ", n);
        }
    } else if (ch == '\b') {
        backspace(t);
        if (sugg_open) build_suggestions(0);
    } else {
        type_char(t, (char)ch);
        /* suggest while typing a word (or an Emmet abbreviation) */
        if (is_word((char)ch) || (lang_emmet(t->lang) && (ch == '.' || ch == '#' || ch == '>' || ch == '*'))) {
            build_suggestions(0);
        } else {
            sugg_open = 0;
        }
    }
    scroll_to_cursor(t);
}

static void click(int action, int arg, int x, int y) {
    tab_t *t = T();
    switch (action) {
    case A_TAB: active = arg; sugg_open = 0; break;
    case A_TAB_CLOSE: close_tab(arg); break;
    case A_WSFILE: open_ws(arg); break;
    case A_HOMEFILE: {
        char p[160];
        str_copy(p, home, sizeof(p));
        str_cat(p, "/", sizeof(p));
        str_cat(p, hfiles[arg], sizeof(p));
        open_home(p);
        break;
    }
    case A_ACT_FILES: show_side = side_search ? 1 : !show_side; side_search = 0; break;
    case A_ACT_SEARCH: show_side = !side_search ? 1 : !show_side; side_search = 1; break;
    case A_SIDE_NEW: run_cmd(CMD_NEW); break;
    case A_SIDE_REFRESH: load_lists(); break;
    case A_PALETTE_ITEM: ov_sel = arg; palette_accept(); break;
    case A_SUGG: sugg_sel = arg; accept_suggestion(); break;
    case A_FIND_NEXT: if (t) find_step(t, 0); break;
    case A_FIND_PREV: if (t) find_step(t, 1); break;
    case A_FIND_CLOSE: find_open = 0; break;
    case A_REPLACE_ALL: if (t) replace_all(t); break;
    case A_STATUS_LANG: open_overlay(OV_LANG); break;
    case A_SEARCH_HIT:
        if (t) {
            t->anchor = arg;
            t->cur = arg + (int)str_len(find_text);
            scroll_to_cursor(t);
        }
        break;
    case A_MINIMAP:
        if (t) {
            t->top = (y - ed_y()) / 2 * arg - rows() / 2;
            if (t->top < 0) t->top = 0;
            if (t->top > nlines - 1) t->top = nlines - 1;
        }
        break;
    case A_TEXT:
        if (t) {
            int l = t->top + (y - ed_y()) / LH, p;
            if (l >= nlines) l = nlines - 1;
            if (l < 0) l = 0;
            p = pos_at_vcol(t, l, (x - text_x() + CW / 2) / CW + t->left);
            if (x < text_x()) p = ls[l];
            if (p == last_click_pos && sh_ticks() - last_click_t < 45) {
                t->cur = p;
                select_word(t);
                last_click_pos = -1;
                return;
            }
            last_click_pos = p;
            last_click_t = sh_ticks();
            t->cur = t->anchor = p;
            t->want_col = -1;
            dragging = 1;
            sugg_open = 0;
        }
        break;
    }
}

int main(int argc, char **argv) {
    ui_event_t ev;
    if (sh_getcwd(home, sizeof(home)) < 0 || !str_starts(home, "/home")) str_copy(home, "/home", sizeof(home));
    load_lists();
    if (argc > 1) {
        if (str_starts(argv[1], "ws:")) {
            const char *id = argv[1] + 3;
            if (*id == '#') id++;
            for (int k = 0; k < nwfiles; k++) {
                if (str_eq(wfiles[k].id, id)) open_ws(k);
            }
        } else {
            open_home(argv[1]);
        }
    }
    if (ui_open("Code", WANT_W, WANT_H) != 0) return 1;
    win_w = ui_surface()->w;
    win_h = ui_surface()->h;
    render();
    for (;;) {
        int redraw = 0;
        while (ui_poll(&ev)) {
            if (ev.type == UI_EV_CLOSE) {
                for (int i = 0; i < MAX_TABS; i++) mem_wipe(tabs[i].buf, sizeof(tabs[i].buf));
                ui_close();
                return 0;
            }
            if (ev.type == UI_EV_KEY) {
                key_byte(ev.key);
                redraw = 1;
            } else if (ev.type == UI_EV_MOUSE) {
                uint32_t pressed = ev.buttons & ~prev_buttons;
                prev_buttons = ev.buttons;
                mouse_x = ev.x;
                mouse_y = ev.y;
                redraw = 1;
                if (pressed & UI_BTN_LEFT) {
                    int hit = -1;
                    for (int i = nhits - 1; i >= 0; i--) {
                        if (rect_contains(hits[i].r, ev.x, ev.y)) {
                            hit = i;
                            break;
                        }
                    }
                    if (overlay != OV_NONE && (hit < 0 || hits[hit].action != A_PALETTE_ITEM)) overlay = OV_NONE;
                    if (hit >= 0) click(hits[hit].action, hits[hit].arg, ev.x, ev.y);
                } else if ((ev.buttons & UI_BTN_LEFT) && dragging && T()) {
                    tab_t *t = T();
                    int l = t->top + (ev.y - ed_y()) / LH;
                    if (l < 0) l = 0;
                    if (l >= nlines) l = nlines - 1;
                    t->cur = pos_at_vcol(t, l, (ev.x - text_x() + CW / 2) / CW + t->left);
                    scroll_to_cursor(t);
                } else if (!(ev.buttons & UI_BTN_LEFT)) {
                    dragging = 0;
                }
            }
        }
        /* a lone Esc (no sequence followed) */
        if (esc_state == 1 && sh_ticks() - esc_t > 6) {
            esc_state = 0;
            handle_key(K_ESC, 0, 0);
            redraw = 1;
        }
        if (message[0] && sh_ticks() - message_t > 800 && sh_ticks() - message_t < 830) redraw = 1;
        if (redraw) render();
        else sh_sleep(1);
    }
}
