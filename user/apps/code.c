/* Qələm ("pen"): Shusha's code and text editor.
 *
 *   code [path | ws:<file id>]
 *
 * Its look is Shusha's own: a library of files on the left (the
 * encrypted workspace, each file showing its owner by rank, and your
 * home), open files as pills in the header with a finder beside them,
 * the text on a card with its own header, footer capsules and a gold
 * current-line bar, an outline of the document on the right with an
 * overview ribbon, and the "Şuşa night" colours taken from the
 * Xarıbülbül emblem.
 *
 * Workspace files are decrypted by workspaced into this process only;
 * saving sends the new text back, where it is re-encrypted, signed and
 * replicated.  The owner and higher ranks can edit; lower ranks with a
 * grant get a read-only card.
 *
 * Editing: syntax colouring with language recognition (code_lang.c),
 * Emmet for HTML/CSS (code_emmet.c), suggestions (Emmet, snippets,
 * keywords, words in the file), auto-closing brackets/quotes/tags,
 * smart indentation, comments, multi-line indent, move/duplicate/delete
 * line, find & replace, go to line, an outline, undo/redo.  Shortcuts
 * are the ones most editors share (see docs/EDITOR.md).
 */
#include "lib.h"
#include "ui.h"
#include "wsclient.h"
#include "code_lang.h"
#include "code_emmet.h"

#define WANT_W 1180
#define WANT_H 630
#define HEAD_H 50
#define LIB_W 228
#define OUT_W 196
#define CARD_HEAD 34
#define FOOT_H 34
#define GUTTER 52
#define RIBBON 12
#define LH 18
#define CW GLYPH_W
#define TABSTOP 4

#define DOC_CAP 32768
#define MAX_TABS 8
#define UNDO_MAX 192
#define POOL_CAP 24576
#define MAX_LINES 6000
#define MAX_SYMS 256

/* "Şuşa night": chrome */
#define C_WIN       0x0e121au
#define C_LIB       0x121822u
#define C_LIB_HOV   0x1a2130u
#define C_CARD      0x151a24u
#define C_CARD_HEAD 0x191f2bu
#define C_EDGE      0x252e3eu
#define C_TEXT      0xd6dde8u
#define C_MUTED     0x7d889cu
#define C_FAINT     0x4c5668u
#define C_GOLD      0xf2cf6bu
#define C_PLUM      0x2b2140u
#define C_PLUM_EDGE 0x6d3c8eu
#define C_LILAC     0xd6b3ffu
#define C_MINT      0x7fe0c2u
#define C_CURLINE   0x1a2030u
#define C_SEL       0x284a5eu
#define C_FIND      0x4e3c12u
#define C_GUIDE     0x283142u
#define C_FIELD     0x1b2230u

/* "Şuşa night": syntax, from the emblem's magenta, gold and green */
static const uint32_t tok_col[T_COUNT] = {
    [T_TEXT] = 0xd6dde8, [T_KEYWORD] = 0xe484f5, [T_CONTROL] = 0xff8fb1, [T_TYPE] = 0x6fe3d0,
    [T_STRING] = 0xbde27f, [T_NUMBER] = 0xffb36b, [T_COMMENT] = 0x5f6b82, [T_FUNC] = 0xf2cf6b,
    [T_PUNCT] = 0x8f9bb0, [T_PREPROC] = 0xff8fb1, [T_TAG] = 0xe484f5, [T_ATTR] = 0xf2cf6b,
    [T_PROP] = 0x6fe3d0, [T_VALUE] = 0xbde27f, [T_SELECTOR] = 0xf2cf6b, [T_HEADING] = 0xe484f5,
    [T_EMPH] = 0xff8fb1, [T_VARIABLE] = 0xc9d6ea,
};

/* Two-letter language badges. */
static const struct { const char *mark; uint32_t bg, fg; } badge[L_COUNT] = {
    [L_PLAIN] = { "Tx", 0x3a4352, 0xd6dde8 }, [L_C] = { "C", 0x3f6fd0, 0xffffff },
    [L_JS] = { "JS", 0xe8c547, 0x1d1a10 },    [L_PY] = { "Py", 0x2f8f85, 0xffffff },
    [L_HTML] = { "<>", 0xe0663f, 0xffffff },  [L_CSS] = { "#", 0x8b5bd6, 0xffffff },
    [L_JSON] = { "{}", 0x6f9e3a, 0xffffff },  [L_MD] = { "M", 0x56637a, 0xffffff },
    [L_SH] = { "$", 0x3c8f4a, 0xffffff },
};

/* Rank colours for owners in the library (admin first). */
static const uint32_t rank_col[] = { 0x8a3fb0, 0x2f8f85, 0x3d6fb0, 0xb0753d, 0x6b7686 };

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

/* workspace / home listings for the library */
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
static int show_lib = 1, show_outline = 1;
static char message[160];
static uint64_t message_t;
static char clip[DOC_CAP];
static int clip_len, clip_line;

/* line table and outline for the active document */
static int ls[MAX_LINES], nlines;
static uint8_t lstate[MAX_LINES];
static uint8_t cls[4096];
typedef struct {
    int line, kind, depth;
    char label[48];
} sym_t;
static sym_t syms[MAX_SYMS];
static int nsyms;

/* overlays: the finder and its prompts */
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
static hit_t hits[192];
static int nhits;

static char reply[WS_MAX_FILE + 2048];

enum {
    A_NONE, A_PILL, A_PILL_CLOSE, A_WSFILE, A_HOMEFILE, A_NEWFILE, A_REFRESH, A_OMNI, A_TEXT, A_RIBBON,
    A_PALETTE_ITEM, A_SUGG, A_FIND_NEXT, A_FIND_PREV, A_FIND_CLOSE, A_FIND_FIELD, A_REPL_FIELD, A_REPL_TOGGLE,
    A_REPLACE_ONE, A_REPLACE_ALL, A_STATUS_LANG, A_OUTLINE, A_CARD_FIND, A_CARD_OUTLINE, A_CARD_SAVE
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

/* ---- layout -------------------------------------------------------------- *
 * header (pills + finder) on top; library left; outline right; between
 * them the document card: card header, text, and a footer row that holds
 * the capsules or, while finding, the find bar. */

static int outline_shown(void) {
    return show_outline && win_w >= 980;
}

static int card_x0(void) {
    return (show_lib ? LIB_W : 0) + 10;
}

static int card_x1(void) {
    return win_w - (outline_shown() ? OUT_W : 0) - 10;
}

static int card_y0(void) {
    return HEAD_H + 6;
}

static int card_y1(void) {
    return win_h - 10;
}

static int foot_h(void) {
    return find_open == 2 ? 72 : find_open ? 42 : FOOT_H;
}

static int ed_y(void) {
    return card_y0() + CARD_HEAD;
}

static int ed_h(void) {
    return card_y1() - ed_y() - foot_h();
}

static int text_x(void) {
    return card_x0() + GUTTER;
}

static int text_w(void) {
    return card_x1() - RIBBON - 8 - text_x();
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

/* In HTML, plain words in text are just words: an abbreviation counts
 * only if it uses Emmet's operators or names a real tag. */
static int emmet_worthy(int lang, const char *a, int n) {
    static const char *const tags[] = { "a", "abbr", "article", "aside", "audio", "b", "blockquote", "body", "br",
        "button", "canvas", "code", "dd", "div", "dl", "dt", "em", "fieldset", "figcaption", "figure", "footer",
        "form", "h1", "h2", "h3", "h4", "h5", "h6", "head", "header", "hr", "html", "i", "iframe", "img", "input",
        "label", "legend", "li", "link", "main", "meta", "nav", "ol", "option", "p", "pre", "script", "section",
        "select", "small", "span", "strong", "style", "sub", "sup", "table", "tbody", "td", "textarea", "tfoot",
        "th", "thead", "title", "tr", "u", "ul", "video", "btn", "bq", "hdr", "ftr", "sect", "art", "fig", 0 };
    char w[24];
    int k = 0;
    if (lang != L_HTML) return 1;
    for (int i = 0; i < n; i++) {
        char c = a[i];
        if (c == '.' || c == '#' || c == '>' || c == '+' || c == '*' || c == '[' || c == '{' || c == '(' ||
            c == '!' || c == '^' || c == '$' || c == ':') return 1;
    }
    if (n >= (int)sizeof(w)) return 0;
    for (k = 0; k < n; k++) w[k] = a[k];
    w[k] = '\0';
    for (int i = 0; tags[i]; i++) {
        if (str_eq(tags[i], w)) return 1;
    }
    return 0;
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
        if (a >= 0 && (ll - a >= 2 || explicit) && emmet_worthy(t->lang, line + a, ll - a) &&
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
    if (a < 0 || !emmet_worthy(t->lang, line + a, ll - a)) return 0;
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
        /* no selection: the whole line */
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

/* ---- actions and the finder ------------------------------------------------- */

typedef struct {
    const char *name, *keys;
    int id;
} cmd_t;

enum {
    CMD_NEW, CMD_SAVE, CMD_SAVEAS, CMD_WSNEW, CMD_CLOSE, CMD_UNDO, CMD_REDO, CMD_FIND, CMD_REPLACE, CMD_GOTO,
    CMD_COMMENT, CMD_DELLINE, CMD_DUPLINE, CMD_MOVEUP, CMD_MOVEDOWN, CMD_SELALL, CMD_EMMET, CMD_SUGGEST,
    CMD_LIBRARY, CMD_OUTLINE, CMD_LANG, CMD_INDENT, CMD_OUTDENT, CMD_REFRESH, CMD_QUICK
};

static const cmd_t cmds[] = {
    { "New file", "Ctrl+N", CMD_NEW }, { "Save", "Ctrl+S", CMD_SAVE },
    { "Save a copy in Home...", "", CMD_SAVEAS }, { "Save to the encrypted workspace...", "", CMD_WSNEW },
    { "Close this file", "Ctrl+W", CMD_CLOSE }, { "Undo", "Ctrl+Z", CMD_UNDO }, { "Redo", "Ctrl+Y", CMD_REDO },
    { "Find in this file", "Ctrl+F", CMD_FIND }, { "Find and replace", "Ctrl+R", CMD_REPLACE },
    { "Go to line...", "Ctrl+G", CMD_GOTO }, { "Comment or uncomment lines", "Ctrl+/", CMD_COMMENT },
    { "Delete line", "Ctrl+Shift+K", CMD_DELLINE }, { "Duplicate line", "Shift+Alt+Down", CMD_DUPLINE },
    { "Move line up", "Alt+Up", CMD_MOVEUP }, { "Move line down", "Alt+Down", CMD_MOVEDOWN },
    { "Select everything", "Ctrl+A", CMD_SELALL }, { "Expand Emmet abbreviation", "Tab", CMD_EMMET },
    { "Show suggestions", "Ctrl+Space", CMD_SUGGEST }, { "Show or hide the library", "Ctrl+B", CMD_LIBRARY },
    { "Show or hide the outline", "Ctrl+Shift+O", CMD_OUTLINE }, { "Change language...", "", CMD_LANG },
    { "Indent lines", "Tab", CMD_INDENT }, { "Outdent lines", "Shift+Tab", CMD_OUTDENT },
    { "Refresh the library", "", CMD_REFRESH }, { "Open a file...", "Ctrl+P", CMD_QUICK },
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

/* One finder result. */
enum { FI_ACTION, FI_WSFILE, FI_HOMEFILE, FI_LANG };
typedef struct {
    int type, id;
    const char *name, *keys;
} fitem_t;

#define MAX_FI 14

static int add_actions(fitem_t *it, int n) {
    for (int i = 0; i < NCMDS && n < MAX_FI; i++) {
        if (ci_contains(cmds[i].name, ov_text)) it[n++] = (fitem_t){ FI_ACTION, cmds[i].id, cmds[i].name, cmds[i].keys };
    }
    return n;
}

static int add_files(fitem_t *it, int n) {
    for (int i = 0; i < nwfiles && n < MAX_FI; i++) {
        if (wfiles[i].open && ci_contains(wfiles[i].name, ov_text)) {
            it[n++] = (fitem_t){ FI_WSFILE, i, wfiles[i].name, "workspace" };
        }
    }
    for (int i = 0; i < nhfiles && n < MAX_FI; i++) {
        if (ci_contains(hfiles[i], ov_text)) it[n++] = (fitem_t){ FI_HOMEFILE, i, hfiles[i], "home" };
    }
    return n;
}

/* F1 shows actions then files; Ctrl+P files then actions. */
static int finder_items(fitem_t *it) {
    int n = 0;
    if (overlay == OV_PALETTE) n = add_files(it, add_actions(it, 0));
    else if (overlay == OV_QUICK) n = add_actions(it, add_files(it, 0));
    else if (overlay == OV_LANG) {
        for (int i = 0; i < L_COUNT && n < MAX_FI; i++) {
            if (ci_contains(lang_name(i), ov_text)) it[n++] = (fitem_t){ FI_LANG, i, lang_name(i), "" };
        }
    }
    return n;
}

static void run_cmd(int id);

static void palette_accept(void) {
    fitem_t it[MAX_FI];
    int n = finder_items(it);
    fitem_t f;
    overlay = OV_NONE;
    if (ov_sel >= n) return;
    f = it[ov_sel];
    if (f.type == FI_ACTION) run_cmd(f.id);
    else if (f.type == FI_WSFILE) open_ws(f.id);
    else if (f.type == FI_HOMEFILE) {
        char p[160];
        str_copy(p, home, sizeof(p));
        str_cat(p, "/", sizeof(p));
        str_cat(p, hfiles[f.id], sizeof(p));
        open_home(p);
    } else if (f.type == FI_LANG && T()) {
        T()->lang = f.id;
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
    case CMD_REFRESH: load_lists(); set_msg("Library refreshed"); return;
    case CMD_LIBRARY: show_lib = !show_lib; return;
    case CMD_OUTLINE: show_outline = !show_outline; return;
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
    case CMD_FIND: find_open = find_open ? find_open : 1; find_focus_replace = 0; break;
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

/* A language badge: rounded square with a short mark. */
static void draw_badge(surface_t *s, int x, int y, int size, int lang) {
    const char *m = badge[lang].mark;
    int tw = text_width_f(m, FONT_BOLD);
    draw_round_fill(s, x, y, size, size, size / 4, badge[lang].bg);
    draw_text_f(s, x + (size - tw) / 2, y + (size - GLYPH_H) / 2, m, badge[lang].fg, FONT_BOLD);
}

/* The pen nib: a gold kite with a slit and a breather hole. */
static void draw_nib(surface_t *s, int cx, int cy, int h) {
    int up = h * 9 / 20, down = h - up, half = h * 7 / 20;
    for (int dy = -up; dy <= down; dy++) {
        int w = dy < 0 ? (up + dy) * half / up : (down - dy) * half / down;
        draw_fill(s, cx - w, cy + dy, 2 * w + 1, 1, dy < 0 ? 0xf7d97f : 0xe9b949);
    }
    draw_fill(s, cx, cy + 2, 1, down - 3, 0x3a2410);
    draw_circle(s, cx, cy, h / 10 + 1, 0x3a2410, 255);
}

/* Keycaps for a shortcut like "Ctrl+Shift+P", right-aligned at x. */
static int draw_keys(surface_t *s, int x, int y, const char *keys) {
    char part[16];
    int n = 0, total = 0, starts[6], lens[6], k = 0;
    if (!keys || !keys[0]) return 0;
    for (int i = 0;; i++) {
        if (keys[i] == '+' || !keys[i]) {
            if (k < 6) {
                starts[k] = i - n;
                lens[k++] = n;
            }
            n = 0;
            if (!keys[i]) break;
        } else {
            n++;
        }
    }
    for (int i = k - 1; i >= 0; i--) {
        int w;
        int c = lens[i] < 15 ? lens[i] : 15;
        mem_copy(part, keys + starts[i], (uint64_t)c);
        part[c] = '\0';
        w = text_width(part) + 12;
        x -= w;
        draw_round_fill(s, x, y, w, 20, 5, 0x3a4558);
        draw_round_fill(s, x, y, w, 18, 5, 0x262e3d);
        draw_text(s, x + 6, y + 1, part, 0xc9d6ea);
        x -= 4;
        total += w + 4;
    }
    return total;
}

static void draw_cross(surface_t *s, int cx, int cy, uint32_t c) {
    for (int k = -3; k <= 3; k++) {
        draw_fill(s, cx + k, cy + k, 1, 1, c);
        draw_fill(s, cx + k, cy - k, 1, 1, c);
    }
}

static void draw_chevron(surface_t *s, int cx, int cy, int up, uint32_t c) {
    for (int k = 0; k < 4; k++) {
        int y = up ? cy - 1 + k : cy + 1 - k;
        draw_fill(s, cx - k, y, 1, 2, c);
        draw_fill(s, cx + k, y, 1, 2, c);
    }
}

static void ghost_button(surface_t *s, int x, int y, int w, const char *label, int strong, int action) {
    int h = 22, on = hov(x, y, w, h);
    uint32_t fill = strong ? (on ? 0x8a4fb0 : 0x6d3c8e) : (on ? 0x2b3446 : 0x212937);
    draw_round_fill(s, x, y, w, h, 11, fill);
    draw_text(s, x + (w - text_width(label)) / 2, y + 3, label, strong ? 0xffffff : C_TEXT);
    add_hit(x, y, w, h, action, 0);
}

/* ---- header: brand, document pills, finder ------------------------------------ */

static void draw_header(surface_t *s) {
    int omni_w = 250, ox = win_w - omni_w - 12, x = card_x0(), shown = 0, hidden = 0;
    draw_vgradient(s, 0, 0, win_w, HEAD_H, 0x1c1430, 0x131925);
    draw_fill(s, 0, HEAD_H - 1, win_w, 1, 0x241c35);
    if (show_lib || x < 120) {
        draw_nib(s, 24, 24, 22);
        draw_text_f(s, 42, 16, "Qələm", 0xf3e9ff, FONT_BOLD);
        if (!show_lib) x = 120;
    }
    for (int i = 0; i < MAX_TABS; i++) {
        tab_t *t = &tabs[i];
        int w, on = i == active, h = 30, y = 10;
        if (!t->used) continue;
        w = text_width(t->title) + 76;
        if (w > 210) w = 210;
        if (x + w > ox - 50) {
            hidden++;
            continue;
        }
        draw_round_fill(s, x, y, w, h, 15, on ? C_PLUM_EDGE : 0x232b3a);
        draw_round_fill(s, x + 1, y + 1, w - 2, h - 2, 14, on ? C_PLUM : (hov(x, y, w, h) ? 0x1f2635 : 0x19202c));
        if (t->ws_id[0]) {
            draw_round_fill(s, x + 5, y + 5, 20, 20, 10, 0x3b2a55);
            draw_icon(s, x + 7, y + 7, ICON_LOCK);
        } else {
            draw_badge(s, x + 5, y + 5, 20, t->lang);
        }
        draw_text_clip(s, x + 32, y + 7, t->title, on ? 0xffffff : 0xa3adbf, w - 62);
        if (t->dirty && !hov(x + w - 26, y + 4, 22, 22)) {
            draw_circle(s, x + w - 15, y + h / 2, 4, C_GOLD, 255);
        } else {
            uint32_t c = hov(x + w - 26, y + 4, 22, 22) ? 0xffffff : 0x7d889c;
            if (hov(x + w - 26, y + 4, 22, 22)) draw_circle(s, x + w - 15, y + h / 2, 9, 0x3a3150, 255);
            draw_cross(s, x + w - 15, y + h / 2, c);
        }
        add_hit(x + w - 26, y + 4, 22, 22, A_PILL_CLOSE, i);
        add_hit(x, y, w, h, A_PILL, i);
        x += w + 8;
        shown++;
    }
    if (hidden) {
        char more[12], n[8];
        str_copy(more, "+", sizeof(more));
        str_cat(more, fmt_u64(n, (uint64_t)hidden, 10), sizeof(more));
        draw_round_fill(s, x, 14, text_width(more) + 16, 22, 11, 0x232b3a);
        draw_text(s, x + 8, 17, more, C_MUTED);
    }
    (void)shown;
    /* the finder field */
    {
        int on = hov(ox, 10, omni_w, 30);
        draw_round_fill(s, ox, 10, omni_w, 30, 15, on ? 0x3a3150 : 0x2a2440);
        draw_round_fill(s, ox + 1, 11, omni_w - 2, 28, 14, 0x161b26);
        draw_rrect_outline(s, ox + 12, 18, 10, 10, 5, 0x8f9bb0, 255);
        draw_fill(s, ox + 21, 27, 3, 2, 0x8f9bb0);
        draw_text(s, ox + 30, 17, "Find a file or action", 0x7d889c);
        draw_keys(s, ox + omni_w - 8, 15, "F1");
        add_hit(ox, 10, omni_w, 30, A_OMNI, 0);
    }
}

/* ---- library ------------------------------------------------------------------ */

static void lib_row(surface_t *s, int y, int on, int action, int arg) {
    if (on) {
        draw_round_fill(s, 8, y, LIB_W - 16, 26, 7, C_PLUM);
        draw_fill(s, 8, y + 6, 3, 14, C_GOLD);
    } else if (hov(8, y, LIB_W - 16, 26)) {
        draw_round_fill(s, 8, y, LIB_W - 16, 26, 7, C_LIB_HOV);
    }
    add_hit(8, y, LIB_W - 16, 26, action, arg);
}

static void draw_library(surface_t *s) {
    tab_t *t = T();
    int y = HEAD_H + 14;
    draw_fill(s, 0, HEAD_H, LIB_W, win_h - HEAD_H, C_LIB);
    draw_fill(s, LIB_W - 1, HEAD_H, 1, win_h - HEAD_H, 0x1d2430);
    if (ws_active) {
        char sub[64], n[8];
        draw_circle(s, 20, y + 8, 5, 0xb44fd8, 255);
        draw_text_f(s, 32, y, "Workspace", C_TEXT, FONT_BOLD);
        str_copy(sub, ws_name, sizeof(sub));
        str_cat(sub, my_rank == 0 ? " \xc2\xb7 admin" : " \xc2\xb7 rank ", sizeof(sub));
        if (my_rank) str_cat(sub, fmt_u64(n, (uint64_t)my_rank, 10), sizeof(sub));
        draw_text_clip(s, 32, y + 19, sub, C_LILAC, LIB_W - 70);
        {
            int bx = LIB_W - 36, on = hov(bx, y, 24, 24);
            draw_circle(s, bx + 12, y + 10, 11, on ? 0x6d3c8e : 0x2b2140, 255);
            draw_fill(s, bx + 8, y + 10, 9, 1, C_LILAC);
            draw_fill(s, bx + 12, y + 6, 1, 9, C_LILAC);
            add_hit(bx, y - 2, 24, 24, A_NEWFILE, 0);
        }
        y += 44;
        for (int i = 0; i < nwfiles && y < win_h - 150; i++) {
            wfile_t *f = &wfiles[i];
            int on = t && str_eq(t->ws_id, f->id), r = f->owner_rank < 4 ? f->owner_rank : 4;
            char ini[2] = { (char)(f->owner[0] >= 'a' && f->owner[0] <= 'z' ? f->owner[0] - 32 : f->owner[0]), 0 };
            lib_row(s, y, on, A_WSFILE, i);
            if (f->open) draw_badge(s, 16, y + 4, 18, lang_detect(f->name, "", 0));
            else draw_icon(s, 17, y + 5, ICON_LOCK);
            draw_text_clip(s, 42, y + 5, f->open ? f->name : "hidden name", f->open ? C_TEXT : C_FAINT, LIB_W - 86);
            draw_circle(s, LIB_W - 28, y + 13, 9, rank_col[r], 255);
            draw_text_f(s, LIB_W - 28 - text_width_f(ini, FONT_BOLD) / 2, y + 5, ini, 0xffffff, FONT_BOLD);
            y += 28;
        }
        if (!nwfiles) {
            draw_text(s, 32, y, "Nothing shared yet.", C_FAINT);
            y += 24;
        }
        y += 14;
    }
    draw_circle(s, 20, y + 8, 5, C_MINT, 255);
    draw_text_f(s, 32, y, "Home", C_TEXT, FONT_BOLD);
    draw_text_clip(s, 32, y + 19, "private to you", 0x5fae9a, LIB_W - 70);
    {
        int bx = LIB_W - 36, on = hov(bx, y, 24, 24);
        draw_circle(s, bx + 12, y + 10, 11, on ? 0x245a50 : 0x15302b, 255);
        draw_rrect_outline(s, bx + 7, y + 5, 10, 10, 5, C_MINT, 255);
        add_hit(bx, y - 2, 24, 24, A_REFRESH, 0);
    }
    y += 44;
    for (int i = 0; i < nhfiles && y < win_h - 90; i++) {
        char p[160];
        int on;
        str_copy(p, home, sizeof(p));
        str_cat(p, "/", sizeof(p));
        str_cat(p, hfiles[i], sizeof(p));
        on = t && str_eq(t->path, p);
        lib_row(s, y, on, A_HOMEFILE, i);
        draw_badge(s, 16, y + 4, 18, lang_detect(hfiles[i], "", 0));
        draw_text_clip(s, 42, y + 5, hfiles[i], C_TEXT, LIB_W - 56);
        y += 28;
    }
    if (!nhfiles) draw_text(s, 32, y, "No files yet.", C_FAINT);
    /* the promise, at the bottom */
    {
        int by = win_h - 58;
        draw_round_fill(s, 10, by, LIB_W - 20, 46, 9, 0x171d28);
        draw_icon(s, 20, by + 8, ICON_LOCK);
        draw_text(s, 42, by + 6, "Workspace files are", C_MUTED);
        draw_text(s, 42, by + 24, "decrypted in memory only", C_MUTED);
    }
}

/* ---- outline ------------------------------------------------------------------- */

static void build_outline(tab_t *t) {
    char label[48];
    nsyms = 0;
    for (int l = 0; l < nlines && nsyms < MAX_SYMS; l++) {
        int st = ls[l], n = line_end(t, l) - st, kind, depth = 0;
        if (n <= 0) continue;
        if (n > 300) n = 300;
        kind = lang_symbol(t->lang, t->buf + st, n, label, sizeof(label));
        if (!kind) continue;
        for (int i = st; i < st + n && (t->buf[i] == ' ' || t->buf[i] == '\t'); i++) depth += t->buf[i] == '\t' ? 4 : 1;
        syms[nsyms].line = l;
        syms[nsyms].kind = kind;
        syms[nsyms].depth = depth / 4 > 3 ? 3 : depth / 4;
        str_copy(syms[nsyms].label, label, sizeof(syms[nsyms].label));
        nsyms++;
    }
}

static void draw_outline(surface_t *s, tab_t *t) {
    static const char *const glyph[] = { "", "f", "C", "H", "<", "{", "k", "#" };
    static const uint32_t gcol[] = { 0, 0xf2cf6b, 0x6fe3d0, 0xe484f5, 0xe484f5, 0x6fe3d0, 0xffb36b, 0xff8fb1 };
    int x = win_w - OUT_W, y = HEAD_H + 14, cur = -1, cl = t ? line_of(t->cur) : 0;
    char n[8];
    draw_fill(s, x, HEAD_H, OUT_W, win_h - HEAD_H, C_LIB);
    draw_fill(s, x, HEAD_H, 1, win_h - HEAD_H, 0x1d2430);
    draw_text_f(s, x + 14, y, "Outline", C_TEXT, FONT_BOLD);
    if (t) {
        fmt_u64(n, (uint64_t)nsyms, 10);
        draw_round_fill(s, x + 80, y, text_width(n) + 14, 18, 9, 0x232b3a);
        draw_text(s, x + 87, y + 1, n, C_MUTED);
    }
    y += 30;
    if (!t) {
        draw_text(s, x + 14, y, "Open a file to see", C_FAINT);
        draw_text(s, x + 14, y + 18, "its structure here.", C_FAINT);
        return;
    }
    if (!nsyms) {
        draw_text_clip(s, x + 14, y, "Nothing to outline in", C_FAINT, OUT_W - 20);
        draw_text_clip(s, x + 14, y + 18, lang_name(t->lang), C_FAINT, OUT_W - 20);
        return;
    }
    for (int i = 0; i < nsyms; i++) {
        if (syms[i].line <= cl) cur = i;
    }
    for (int i = 0; i < nsyms && y < win_h - 30; i++) {
        int ix = x + 10 + syms[i].depth * 12, on = i == cur, k = syms[i].kind;
        if (on) {
            draw_round_fill(s, x + 6, y - 2, OUT_W - 12, 24, 6, C_PLUM);
            draw_fill(s, x + 6, y + 3, 3, 14, C_GOLD);
        } else if (hov(x + 6, y - 2, OUT_W - 12, 24)) {
            draw_round_fill(s, x + 6, y - 2, OUT_W - 12, 24, 6, C_LIB_HOV);
        }
        draw_round_fill(s, ix + 4, y + 1, 18, 18, 5, 0x232b3a);
        draw_text_f(s, ix + 13 - text_width_f(glyph[k], FONT_BOLD) / 2, y + 2, glyph[k], gcol[k], FONT_BOLD);
        draw_text_clip(s, ix + 28, y + 2, syms[i].label, on ? 0xffffff : C_TEXT, OUT_W - (ix - x) - 36);
        add_hit(x + 6, y - 2, OUT_W - 12, 24, A_OUTLINE, syms[i].line);
        y += 26;
    }
}

/* ---- the document card ------------------------------------------------------------ */

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

static void draw_card_frame(surface_t *s) {
    int x0 = card_x0(), y0 = card_y0(), w = card_x1() - x0, h = card_y1() - y0;
    draw_shadow(s, x0, y0 + 4, w, h, 10, 14, 0x000000, 120);
    draw_round_fill(s, x0, y0, w, h, 10, C_EDGE);
    draw_round_fill(s, x0 + 1, y0 + 1, w - 2, h - 2, 9, C_CARD);
    draw_rrect(s, x0 + 1, y0 + 1, w - 2, CARD_HEAD - 1, 9, RR_TL | RR_TR, C_CARD_HEAD, 255);
    draw_fill(s, x0 + 1, y0 + CARD_HEAD - 1, w - 2, 1, 0x1f2633);
}

static void draw_card_head(surface_t *s, tab_t *t) {
    int x0 = card_x0(), y0 = card_y0(), x = x0 + 14, bx = card_x1() - 12;
    if (t->ws_id[0]) {
        draw_icon(s, x, y0 + 9, ICON_LOCK);
        x += 22;
        x += draw_text_f(s, x, y0 + 8, ws_name, C_LILAC, FONT_SANS);
        x += draw_text_f(s, x, y0 + 8, "  /  ", C_FAINT, FONT_SANS);
    } else if (t->path[0]) {
        x += draw_text_f(s, x, y0 + 8, "~  /  ", C_FAINT, FONT_SANS);
    } else {
        x += draw_text_f(s, x, y0 + 8, "unsaved  /  ", C_FAINT, FONT_SANS);
    }
    draw_text_clip_f(s, x, y0 + 8, t->title, 0xffffff, bx - x - 230, FONT_BOLD);
    bx -= 64;
    if (t->readonly) {
        draw_round_fill(s, bx - 18, y0 + 6, 82, 22, 11, 0x3a2f12);
        draw_text(s, bx - 6, y0 + 9, "read-only", 0xf2cf6b);
    } else {
        ghost_button(s, bx, y0 + 6, 60, "Save", t->dirty, A_CARD_SAVE);
    }
    bx -= 92;
    ghost_button(s, bx, y0 + 6, 80, "Outline", outline_shown(), A_CARD_OUTLINE);
    bx -= 64;
    ghost_button(s, bx, y0 + 6, 56, "Find", find_open != 0, A_CARD_FIND);
}

static void draw_text_area(surface_t *s, tab_t *t) {
    int x0 = card_x0(), x1 = card_x1(), y0 = ed_y(), tx = text_x(), tw = text_w(), nrows = rows();
    int cl = line_of(t->cur), sa, sb, has_sel = sel_range(t, &sa, &sb), ba = -1, bb = -1, fl = (int)str_len(find_text);
    if (!has_sel) (void)bracket_match(t, &ba, &bb);
    for (int r = 0; r < nrows && t->top + r < nlines; r++) {
        int l = t->top + r, y = y0 + r * LH, st = ls[l], en = line_end(t, l), n = en - st, col = 0, guide = 0;
        char num[12];
        if (l == cl && !has_sel) {
            draw_fill(s, x0 + 1, y, x1 - x0 - RIBBON - 6, LH, C_CURLINE);
            draw_fill(s, x0 + 1, y + 2, 3, LH - 4, C_GOLD);
        }
        fmt_u64(num, (uint64_t)l + 1, 10);
        draw_text_f(s, tx - 14 - text_width_f(num, FONT_MONO), y + 1, num, l == cl ? C_GOLD : C_FAINT, FONT_MONO);
        if (n > (int)sizeof(cls)) n = (int)sizeof(cls);
        lang_line(t->lang, t->buf + st, n, lstate[l], cls);
        for (int i = 0; i < n; i++) {
            char c = t->buf[st + i];
            int w = c == '\t' ? (col / TABSTOP + 1) * TABSTOP - col : 1, px = tx + (col - t->left) * CW, pos = st + i;
            if (!guide && c != ' ' && c != '\t') guide = col;
            if (col + w > t->left && px < tx + tw) {
                if (has_sel && pos >= sa && pos < sb) draw_fill(s, px, y, w * CW, LH, C_SEL);
                else if (find_open && fl && match_at(t, pos)) {
                    draw_fill(s, px, y + 1, fl * CW, LH - 2, C_FIND);
                    draw_fill(s, px, y + LH - 2, fl * CW, 1, 0xffb36b);
                }
                if (pos == ba || pos == bb) draw_fill(s, px, y + LH - 2, CW, 2, C_GOLD);
                if (c != ' ' && c != '\t' && col >= t->left) draw_char(s, px, y + 1, c, tok_col[cls[i] < T_COUNT ? cls[i] : 0]);
            }
            col += w;
        }
        if (!guide && n) guide = col;
        /* dotted indentation guides */
        for (int g = TABSTOP; g < guide; g += TABSTOP) {
            int gx = tx + (g - t->left) * CW;
            if (gx <= tx) continue;
            for (int d = 0; d < LH; d += 3) draw_fill(s, gx, y + d, 1, 1, l == cl ? 0x55607a : C_GUIDE);
        }
        if (has_sel && sa <= en && sb > en && en < t->len) {
            int px = tx + (col - t->left) * CW;
            if (px >= tx) draw_fill(s, px, y, CW, LH, C_SEL);
        }
    }
    /* the caret */
    {
        int r = cl - t->top, c = vcol(t, t->cur) - t->left;
        if (r >= 0 && r < nrows && c >= 0 && overlay == OV_NONE) draw_fill(s, tx + c * CW, y0 + r * LH + 1, 2, LH - 2, 0xffe6a0);
    }
    add_hit(x0, y0, tx + tw - x0, ed_h(), A_TEXT, 0);

    /* overview ribbon: where you are, and where the matches are */
    {
        int rx = x1 - RIBBON - 2, rh = ed_h(), lines = nlines > 1 ? nlines : 1;
        draw_round_fill(s, rx + 3, y0 + 2, 6, rh - 4, 3, 0x1b2230);
        {
            int vy = y0 + 2 + (rh - 4) * t->top / lines, vh = (rh - 4) * nrows / lines;
            if (vh < 10) vh = 10;
            if (vy + vh > y0 + rh - 2) vh = y0 + rh - 2 - vy;
            draw_round_fill(s, rx + 3, vy, 6, vh, 3, hov(rx, y0, RIBBON + 2, rh) ? 0x4a5468 : 0x343d4f);
        }
        if (find_open && fl) {
            for (int i = 0; i < t->len; i++) {
                if (match_at(t, i)) draw_fill(s, rx + 2, y0 + 2 + (rh - 4) * line_of(i) / lines, 8, 2, 0xffb36b);
            }
        }
        draw_fill(s, rx + 1, y0 + 2 + (rh - 4) * cl / lines, 10, 2, C_GOLD);
        add_hit(rx, y0, RIBBON + 2, rh, A_RIBBON, 0);
    }
}

static int capsule(surface_t *s, int x, int y, const char *text, uint32_t fill, uint32_t edge, uint32_t fg) {
    int w = text_width(text) + 22;
    draw_round_fill(s, x, y, w, 22, 11, edge);
    draw_round_fill(s, x + 1, y + 1, w - 2, 20, 10, fill);
    draw_text(s, x + 11, y + 3, text, fg);
    return w;
}

static void draw_foot(surface_t *s, tab_t *t) {
    int y = card_y1() - FOOT_H + 6, x = card_x0() + 12, rx = card_x1() - 14;
    char pos[48], n[12];
    if (t->ws_id[0]) {
        char loc[64];
        str_copy(loc, "   encrypted  -  ", sizeof(loc));
        str_cat(loc, ws_name, sizeof(loc));
        capsule(s, x, y, loc, C_PLUM, C_PLUM_EDGE, C_LILAC);
        draw_icon(s, x + 8, y + 3, ICON_LOCK);
    } else {
        capsule(s, x, y, t->path[0] ? "   private  -  home" : "   not saved yet", 0x15302b, 0x245a50, C_MINT);
        draw_circle(s, x + 14, y + 11, 4, C_MINT, 255);
    }
    /* right: position, language, indentation, encoding */
    str_copy(pos, "Ln ", sizeof(pos));
    str_cat(pos, fmt_u64(n, (uint64_t)line_of(t->cur) + 1, 10), sizeof(pos));
    str_cat(pos, ", Col ", sizeof(pos));
    str_cat(pos, fmt_u64(n, (uint64_t)vcol(t, t->cur) + 1, 10), sizeof(pos));
    {
        const char *lname = lang_name(t->lang);
        int w = text_width(pos) + text_width(lname) + text_width("4 spaces") + text_width("UTF-8") + 4 * 26 + 30;
        int cx = rx - w;
        draw_round_fill(s, cx, y, w, 22, 11, 0x2a3344);
        draw_round_fill(s, cx + 1, y + 1, w - 2, 20, 10, 0x1b2230);
        cx += 12;
        cx += draw_text_f(s, cx, y + 3, pos, C_TEXT, FONT_SANS) + 12;
        draw_fill(s, cx - 6, y + 6, 1, 10, 0x3a4558);
        draw_badge(s, cx, y + 3, 16, t->lang);
        if (hov(cx - 4, y, text_width(lname) + 28, 22)) draw_round_fill(s, cx + 18, y + 2, text_width(lname) + 6, 18, 5, 0x2b3446);
        add_hit(cx - 4, y, text_width(lname) + 28, 22, A_STATUS_LANG, 0);
        cx += 20;
        cx += draw_text_f(s, cx + 1, y + 3, lname, C_TEXT, FONT_SANS) + 14;
        draw_fill(s, cx - 6, y + 6, 1, 10, 0x3a4558);
        cx += draw_text_f(s, cx, y + 3, "4 spaces", C_MUTED, FONT_SANS) + 12;
        draw_fill(s, cx - 6, y + 6, 1, 10, 0x3a4558);
        draw_text_f(s, cx, y + 3, "UTF-8", C_MUTED, FONT_SANS);
        if (lang_emmet(t->lang)) capsule(s, rx - w - 72, y, "Emmet", 0x1d3320, 0x2f5a36, 0x9be59f);
    }
}

static void draw_toast(surface_t *s) {
    int w, x, y;
    if (!message[0] || sh_ticks() - message_t > 400) return;
    w = text_width(message) + 44;
    if (w > card_x1() - card_x0() - 40) w = card_x1() - card_x0() - 40;
    x = card_x0() + (card_x1() - card_x0() - w) / 2;
    y = card_y1() - foot_h() - 44;
    draw_shadow(s, x, y + 3, w, 32, 16, 10, 0x000000, 140);
    draw_round_fill(s, x, y, w, 32, 16, 0x3a3150);
    draw_round_fill(s, x + 1, y + 1, w - 2, 30, 15, 0x221d33);
    draw_circle(s, x + 16, y + 16, 4, C_GOLD, 255);
    draw_text_clip(s, x + 28, y + 8, message, 0xf3e9ff, w - 40);
}

static void field(surface_t *s, int x, int y, int w, const char *text, const char *ph, int focused, int action) {
    draw_round_fill(s, x, y, w, 26, 8, focused ? 0x6d3c8e : 0x2a3344);
    draw_round_fill(s, x + 1, y + 1, w - 2, 24, 7, C_FIELD);
    draw_text_clip(s, x + 10, y + 5, text[0] ? text : ph, text[0] ? C_TEXT : C_FAINT, w - 20);
    if (focused) {
        int cx = x + 10 + text_width(text);
        draw_fill(s, cx < x + w - 6 ? cx : x + w - 6, y + 5, 2, 16, C_GOLD);
    }
    add_hit(x, y, w, 26, action, 0);
}

static void round_icon_button(surface_t *s, int cx, int cy, int action, int kind) {
    int on = hov(cx - 12, cy - 12, 24, 24);
    draw_circle(s, cx, cy, 11, on ? 0x3a3150 : 0x222a38, 255);
    if (kind == 0) draw_chevron(s, cx, cy, 1, C_TEXT);
    else if (kind == 1) draw_chevron(s, cx, cy, 0, C_TEXT);
    else draw_cross(s, cx, cy, C_TEXT);
    add_hit(cx - 12, cy - 12, 24, 24, action, 0);
}

static void draw_find_bar(surface_t *s, tab_t *t) {
    int x0 = card_x0() + 1, w = card_x1() - card_x0() - 2, y = card_y1() - foot_h(), x = x0 + 14;
    int idx, total = count_matches(t, &idx);
    char cnt[32], n[12];
    draw_fill(s, x0, y, w, foot_h() - 8, 0x181e2a);
    draw_fill(s, x0, y, w, 1, 0x2a3344);
    draw_text(s, x, y + 12, "Find", C_MUTED);
    field(s, x + 52, y + 8, 260, find_text, "text to look for", !find_focus_replace, A_FIND_FIELD);
    if (!find_text[0]) str_copy(cnt, "", sizeof(cnt));
    else if (!total) str_copy(cnt, "no matches", sizeof(cnt));
    else {
        str_copy(cnt, fmt_u64(n, (uint64_t)(idx ? idx : 1), 10), sizeof(cnt));
        str_cat(cnt, " of ", sizeof(cnt));
        str_cat(cnt, fmt_u64(n, (uint64_t)total, 10), sizeof(cnt));
    }
    draw_text(s, x + 324, y + 12, cnt, total || !find_text[0] ? C_TEXT : 0xff8fb1);
    round_icon_button(s, x + 418, y + 21, A_FIND_PREV, 0);
    round_icon_button(s, x + 446, y + 21, A_FIND_NEXT, 1);
    ghost_button(s, x + 470, y + 10, 84, find_open == 2 ? "Replace -" : "Replace +", 0, A_REPL_TOGGLE);
    round_icon_button(s, x0 + w - 22, y + 21, A_FIND_CLOSE, 2);
    if (find_open == 2) {
        draw_text(s, x, y + 42, "With", C_MUTED);
        field(s, x + 52, y + 38, 260, repl_text, "replacement", find_focus_replace, A_REPL_FIELD);
        ghost_button(s, x + 324, y + 40, 76, "Replace", 0, A_REPLACE_ONE);
        ghost_button(s, x + 408, y + 40, 60, "All", 1, A_REPLACE_ALL);
    }
}

/* ---- suggestions ---------------------------------------------------------------- */

static void draw_suggest(surface_t *s, tab_t *t) {
    static const char *const kinds[] = { "emmet", "snippet", "keyword", "word" };
    static const uint32_t kfill[] = { 0x1d3320, 0x3a2f12, 0x34204a, 0x232b3a };
    static const uint32_t kfg[] = { 0x9be59f, 0xf2cf6b, 0xe484f5, 0xa3adbf };
    int r = line_of(t->cur) - t->top, cx = text_x() + (vcol(t, sugg_from) - t->left) * CW - 6;
    int lw = 300, rowh = 26, h = nsugg * rowh + 8, y = ed_y() + (r + 1) * LH + 4, preview = 0, pw = 300;
    if (nsugg && (sugg[sugg_sel].kind == S_EMMET || sugg[sugg_sel].kind == S_SNIPPET)) preview = 1;
    if (preview && h < 9 * 16 + 30) h = 9 * 16 + 30;
    if (cx + lw + (preview ? pw : 0) > card_x1() - 10) cx = card_x1() - 10 - lw - (preview ? pw : 0);
    if (y + h > card_y1() - 8) y = ed_y() + r * LH - h - 4;
    draw_shadow(s, cx, y + 4, lw + (preview ? pw : 0), h, 10, 14, 0x000000, 150);
    draw_round_fill(s, cx, y, lw + (preview ? pw : 0), h, 10, 0x323e52);
    draw_round_fill(s, cx + 1, y + 1, lw + (preview ? pw : 0) - 2, h - 2, 9, 0x1a2130);
    for (int i = 0; i < nsugg; i++) {
        int yy = y + 4 + i * rowh, k = sugg[i].kind, kw = text_width(kinds[k]) + 14;
        if (i == sugg_sel) {
            draw_round_fill(s, cx + 4, yy, lw - 8, rowh - 2, 7, C_PLUM);
            draw_fill(s, cx + 4, yy + 6, 3, 12, C_GOLD);
        }
        draw_round_fill(s, cx + 12, yy + 4, kw, 18, 9, kfill[k]);
        draw_text(s, cx + 19, yy + 5, kinds[k], kfg[k]);
        draw_text_clip_f(s, cx + 22 + kw, yy + 5, sugg[i].label, i == sugg_sel ? 0xffffff : C_TEXT, lw - kw - 34, FONT_MONO);
        add_hit(cx, yy, lw, rowh, A_SUGG, i);
    }
    if (preview) {
        const char *p = sugg[sugg_sel].insert;
        int px = cx + lw, py = y + 10;
        char ind[64];
        int il = indent_of(t, line_of(t->cur), ind, sizeof(ind));
        draw_fill(s, px, y + 8, 1, h - 16, 0x2a3344);
        draw_text(s, px + 14, py, "becomes", C_MUTED);
        py += 22;
        for (int k = 0; *p && k < 8; k++) {
            char row[120];
            int j = 0;
            if (k) {
                for (int q = 0; q < il && (*p == ' ' || *p == '\t'); q++) p++;
            }
            while (*p && *p != '\n' && j < 110) {
                if (*p != EMMET_CURSOR) row[j++] = *p;
                p++;
            }
            row[j] = '\0';
            if (*p == '\n') p++;
            draw_text_clip_f(s, px + 14, py + k * 16, row, 0xc9d6ea, pw - 24, FONT_MONO);
        }
    }
}

/* ---- the finder ------------------------------------------------------------------- */

static void draw_finder(surface_t *s) {
    fitem_t it[MAX_FI];
    int n = finder_items(it), w = 600, x = (win_w - w) / 2, y = 64, h, list;
    const char *mode = overlay == OV_PALETTE ? "actions & files" : overlay == OV_QUICK ? "files & actions"
                     : overlay == OV_LANG ? "language" : overlay == OV_GOTO ? "go to line"
                     : overlay == OV_SAVEAS ? "save in home" : "save to workspace";
    const char *ph = overlay == OV_GOTO ? "Line number" : overlay == OV_SAVEAS ? "File name (private to you)"
                   : overlay == OV_WSNEW ? "Name for the new encrypted file" : "Type to search";
    list = overlay == OV_PALETTE || overlay == OV_QUICK || overlay == OV_LANG;
    if (n > 11) n = 11;
    if (ov_sel >= n && n) ov_sel = n - 1;
    h = 64 + (list ? n * 32 + (n ? 34 : 30) : 34);
    draw_fill_a(s, 0, 0, win_w, win_h, 0x05070b, 150);
    draw_shadow(s, x, y + 6, w, h, 14, 20, 0x000000, 170);
    draw_round_fill(s, x, y, w, h, 14, 0x4a3570);
    draw_round_fill(s, x + 1, y + 1, w - 2, h - 2, 13, 0x161b26);
    /* input */
    draw_rrect_outline(s, x + 20, y + 20, 14, 14, 7, C_GOLD, 255);
    draw_fill(s, x + 32, y + 33, 4, 3, C_GOLD);
    draw_text_clip_f(s, x + 46, y + 19, ov_text[0] ? ov_text : ph, ov_text[0] ? 0xffffff : C_FAINT, w - 220,
                     ov_text[0] ? FONT_BOLD : FONT_SANS);
    if (ov_text[0] || 1) {
        int cx = x + 46 + (ov_text[0] ? text_width_f(ov_text, FONT_BOLD) : 0);
        draw_fill(s, cx + 1, y + 19, 2, 17, C_GOLD);
    }
    {
        int mw = text_width(mode) + 20;
        draw_round_fill(s, x + w - mw - 16, y + 17, mw, 22, 11, C_PLUM);
        draw_text(s, x + w - mw - 6, y + 20, mode, C_LILAC);
    }
    draw_fill(s, x + 16, y + 52, w - 32, 1, 0x262e3d);
    if (!list) {
        draw_text(s, x + 22, y + 62, overlay == OV_GOTO ? "Enter to jump there, Esc to cancel"
                                                        : "Enter to save, Esc to cancel", C_MUTED);
        return;
    }
    if (!n) {
        draw_text(s, x + 22, y + 64, "Nothing matches.", C_MUTED);
        return;
    }
    {
        int yy = y + 60, group = -1;
        for (int i = 0; i < n; i++) {
            int g = it[i].type == FI_HOMEFILE ? FI_WSFILE : it[i].type;
            if (g != group && overlay != OV_LANG) {
                draw_text(s, x + 22, yy + 2, g == FI_ACTION ? "Actions" : "Files", C_FAINT);
                yy += 22;
                group = g;
            }
            if (i == ov_sel) {
                draw_round_fill(s, x + 10, yy, w - 20, 30, 8, C_PLUM);
                draw_fill(s, x + 10, yy + 8, 3, 14, C_GOLD);
            } else if (hov(x + 10, yy, w - 20, 30)) {
                draw_round_fill(s, x + 10, yy, w - 20, 30, 8, C_LIB_HOV);
            }
            if (it[i].type == FI_ACTION) {
                draw_round_fill(s, x + 24, yy + 7, 16, 16, 4, 0x2b2140);
                draw_fill(s, x + 29, yy + 12, 6, 6, C_GOLD);
            } else if (it[i].type == FI_WSFILE) {
                draw_round_fill(s, x + 22, yy + 5, 20, 20, 6, 0x3b2a55);
                draw_icon(s, x + 24, yy + 7, ICON_LOCK);
            } else {
                draw_badge(s, x + 22, yy + 5, 20, it[i].type == FI_LANG ? it[i].id : lang_detect(it[i].name, "", 0));
            }
            draw_text_clip(s, x + 52, yy + 6, it[i].name, i == ov_sel ? 0xffffff : C_TEXT, w - 240);
            if (it[i].type == FI_ACTION) draw_keys(s, x + w - 22, yy + 5, it[i].keys);
            else if (it[i].keys && it[i].keys[0]) {
                int tw = text_width(it[i].keys);
                draw_text(s, x + w - 24 - tw, yy + 6, it[i].keys, it[i].type == FI_WSFILE ? C_LILAC : 0x5fae9a);
            }
            add_hit(x + 10, yy, w - 20, 30, A_PALETTE_ITEM, i);
            yy += 32;
        }
    }
}

/* ---- welcome ------------------------------------------------------------------------- */

static void draw_welcome(surface_t *s) {
    int x0 = card_x0(), x1 = card_x1(), cx = (x0 + x1) / 2, y = card_y0() + 60;
    static const char *const tips[][2] = {
        { "Tab", "expands Emmet: ul>li*3, !, div.card" }, { "Ctrl+Space", "suggestions" },
        { "Ctrl+/", "comment lines" }, { "Alt+Up", "move a line" },
    };
    draw_card_frame(s);
    draw_nib(s, cx, y + 20, 56);
    draw_text_f(s, cx - text_width_f("Qələm", FONT_BOLD) / 2, y + 64, "Qələm", 0xf3e9ff, FONT_BOLD);
    {
        const char *tag = "Code, pages and notes - in your home or in the encrypted workspace.";
        draw_text(s, cx - text_width(tag) / 2, y + 86, tag, C_MUTED);
    }
    y += 126;
    {
        static const char *const head[] = { "New file", "Find anything", "Workspace" };
        static const char *const keys[] = { "Ctrl+N", "F1", "" };
        static const int act[] = { A_NEWFILE, A_OMNI, A_OMNI };
        int tw = 170, gap = 16, tx = cx - (3 * tw + 2 * gap) / 2;
        for (int i = 0; i < 3; i++) {
            int x = tx + i * (tw + gap), on = hov(x, y, tw, 84);
            char sub[48], n[8];
            draw_round_fill(s, x, y, tw, 84, 12, on ? C_PLUM_EDGE : 0x262e3d);
            draw_round_fill(s, x + 1, y + 1, tw - 2, 82, 11, on ? C_PLUM : 0x1a2030);
            draw_text_f(s, x + 16, y + 14, head[i], 0xffffff, FONT_BOLD);
            if (i == 2) {
                if (ws_active) {
                    str_copy(sub, fmt_u64(n, (uint64_t)nwfiles, 10), sizeof(sub));
                    str_cat(sub, " shared file(s)", sizeof(sub));
                } else {
                    str_copy(sub, "not joined yet", sizeof(sub));
                }
                draw_text(s, x + 16, y + 40, sub, ws_active ? C_LILAC : C_FAINT);
            } else {
                draw_keys(s, x + tw - 14, y + 50, keys[i]);
                draw_text(s, x + 16, y + 40, i == 0 ? "start writing" : "files and actions", C_MUTED);
            }
            add_hit(x, y, tw, 84, act[i], 0);
        }
    }
    y += 120;
    for (int i = 0; i < 4; i++) {
        int kx = cx - 30, w = draw_keys(s, kx, y + i * 28, tips[i][0]);
        (void)w;
        draw_text(s, cx - 18, y + i * 28 + 1, tips[i][1], C_MUTED);
    }
}

static void render(void) {
    surface_t *s = ui_surface();
    tab_t *t = T();
    nhits = 0;
    draw_fill(s, 0, 0, win_w, win_h, C_WIN);
    if (t) {
        reindex(t);
        build_outline(t);
        draw_card_frame(s);
        draw_text_area(s, t);
        draw_card_head(s, t);
        if (find_open) draw_find_bar(s, t);
        else draw_foot(s, t);
    } else {
        draw_welcome(s);
    }
    draw_header(s);
    if (show_lib) draw_library(s);
    if (outline_shown()) draw_outline(s, t);
    draw_toast(s);
    if (t && sugg_open && overlay == OV_NONE) draw_suggest(s, t);
    if (overlay != OV_NONE) draw_finder(s);
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
    /* the finder */
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
        case 'p': case 'o': run_cmd(CMD_QUICK); return;
        case 'b': run_cmd(CMD_LIBRARY); return;
        case 'w': run_cmd(CMD_CLOSE); return;
        }
    }
    if (k == K_CSIU && ctrl_shift) {
        if (ch == 'o') { run_cmd(CMD_OUTLINE); return; }
        if (ch == 'e') { show_lib = 1; return; }
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
        else if (ch == 'f' && ctrl_shift) run_cmd(CMD_FIND);
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
    case A_PILL: active = arg; sugg_open = 0; break;
    case A_PILL_CLOSE: close_tab(arg); break;
    case A_WSFILE: open_ws(arg); break;
    case A_HOMEFILE: {
        char p[160];
        str_copy(p, home, sizeof(p));
        str_cat(p, "/", sizeof(p));
        str_cat(p, hfiles[arg], sizeof(p));
        open_home(p);
        break;
    }
    case A_NEWFILE: run_cmd(CMD_NEW); break;
    case A_REFRESH: run_cmd(CMD_REFRESH); break;
    case A_OMNI: open_overlay(OV_PALETTE); break;
    case A_PALETTE_ITEM: ov_sel = arg; palette_accept(); break;
    case A_SUGG: sugg_sel = arg; accept_suggestion(); break;
    case A_FIND_NEXT: if (t) find_step(t, 0); break;
    case A_FIND_PREV: if (t) find_step(t, 1); break;
    case A_FIND_CLOSE: find_open = 0; break;
    case A_FIND_FIELD: find_focus_replace = 0; break;
    case A_REPL_FIELD: find_focus_replace = 1; break;
    case A_REPL_TOGGLE: find_open = find_open == 2 ? 1 : 2; find_focus_replace = find_open == 2; break;
    case A_REPLACE_ONE: if (t) replace_one(t); break;
    case A_REPLACE_ALL: if (t) replace_all(t); break;
    case A_STATUS_LANG: open_overlay(OV_LANG); break;
    case A_CARD_FIND: if (find_open) find_open = 0; else run_cmd(CMD_FIND); break;
    case A_CARD_OUTLINE: run_cmd(CMD_OUTLINE); break;
    case A_CARD_SAVE: run_cmd(CMD_SAVE); break;
    case A_OUTLINE:
        if (t) {
            t->cur = t->anchor = ls[arg < nlines ? arg : nlines - 1];
            t->top = arg - rows() / 3;
            if (t->top < 0) t->top = 0;
        }
        break;
    case A_RIBBON:
        if (t) {
            t->top = (y - ed_y()) * nlines / (ed_h() ? ed_h() : 1) - rows() / 2;
            if (t->top > nlines - rows()) t->top = nlines - rows();
            if (t->top < 0) t->top = 0;
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
    if (ui_open("Qələm", WANT_W, WANT_H) != 0) return 1;
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
                    if (overlay != OV_NONE && (hit < 0 || hits[hit].action != A_PALETTE_ITEM)) {
                        overlay = OV_NONE;
                        hit = -1; /* a click outside the finder only closes it */
                    }
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
        /* the toast goes away by itself */
        if (message[0] && sh_ticks() - message_t > 400 && sh_ticks() - message_t < 430) redraw = 1;
        if (redraw) render();
        else sh_sleep(1);
    }
}
