/* Emmet abbreviation expansion (HTML and CSS) for the Code editor.
 *
 * HTML abbreviations are parsed into a small tree - elements with id,
 * classes, attributes, text and a repeat count, joined by > (child),
 * + (sibling), ^ (climb) and ( ) groups - and printed with indentation.
 * $ in a name, attribute or text becomes the repeat index ($$ pads it).
 * An element without a name takes the usual implicit tag (li in a list,
 * td in a row, span inside inline elements, div elsewhere). */
#include "lib.h"
#include "code_emmet.h"

#define MAXN 160

typedef struct {
    char tag[24], id[40], cls[96], attrs[160], text[160];
    int mult, parent, child, next, group;
} node_t;

static node_t pool[MAXN];
static int npool;
static const char *src;
static int pos, end;

static int new_node(int parent) {
    node_t *n;
    if (npool >= MAXN) return -1;
    n = &pool[npool];
    mem_set(n, 0, sizeof(*n));
    n->mult = 1;
    n->parent = parent;
    n->child = n->next = -1;
    if (parent >= 0) {
        int *link = &pool[parent].child;
        while (*link >= 0) link = &pool[*link].next;
        *link = npool;
    }
    return npool++;
}

static int name_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == ':' ||
           c == '!' || c == '_' || c == '$' || c == '@';
}

static void append(char *dst, int cap, const char *s, int n) {
    int len = (int)str_len(dst);
    for (int i = 0; i < n && len + 1 < cap; i++) dst[len++] = s[i];
    dst[len] = '\0';
}

/* name[=value] pairs inside [ ] */
static void parse_attrs(node_t *n) {
    while (pos < end && src[pos] != ']') {
        int a0, a1, v0 = -1, v1 = -1;
        while (pos < end && src[pos] == ' ') pos++;
        a0 = pos;
        while (pos < end && src[pos] != '=' && src[pos] != ' ' && src[pos] != ']') pos++;
        a1 = pos;
        if (pos < end && src[pos] == '=') {
            pos++;
            if (pos < end && (src[pos] == '"' || src[pos] == '\'')) {
                char q = src[pos++];
                v0 = pos;
                while (pos < end && src[pos] != q) pos++;
                v1 = pos;
                if (pos < end) pos++;
            } else {
                v0 = pos;
                while (pos < end && src[pos] != ' ' && src[pos] != ']') pos++;
                v1 = pos;
            }
        }
        if (a1 > a0) {
            append(n->attrs, sizeof(n->attrs), " ", 1);
            append(n->attrs, sizeof(n->attrs), src + a0, a1 - a0);
            append(n->attrs, sizeof(n->attrs), "=\"", 2);
            if (v0 >= 0) append(n->attrs, sizeof(n->attrs), src + v0, v1 - v0);
            append(n->attrs, sizeof(n->attrs), "\"", 1);
        }
    }
    if (pos < end) pos++;
}

static int parse_seq(int parent);

/* One element or group with its modifiers; returns its node or -1. */
static int parse_item(int parent) {
    int id = new_node(parent), n0;
    node_t *n;
    if (id < 0) return -1;
    n = &pool[id];
    if (pos < end && src[pos] == '(') {
        pos++;
        n->group = 1;
        if (parse_seq(id) < 0) return -1;
        if (pos >= end || src[pos] != ')') return -1;
        pos++;
    } else {
        n0 = pos;
        while (pos < end && name_char(src[pos])) pos++;
        append(n->tag, sizeof(n->tag), src + n0, pos - n0);
        for (;;) {
            if (pos < end && (src[pos] == '#' || src[pos] == '.')) {
                char kind = src[pos++];
                int s0 = pos;
                while (pos < end && name_char(src[pos])) pos++;
                if (kind == '#') {
                    n->id[0] = '\0';
                    append(n->id, sizeof(n->id), src + s0, pos - s0);
                } else {
                    if (n->cls[0]) append(n->cls, sizeof(n->cls), " ", 1);
                    append(n->cls, sizeof(n->cls), src + s0, pos - s0);
                }
            } else if (pos < end && src[pos] == '[') {
                pos++;
                parse_attrs(n);
            } else if (pos < end && src[pos] == '{') {
                int depth = 1, s0 = ++pos;
                while (pos < end && depth) {
                    if (src[pos] == '{') depth++;
                    if (src[pos] == '}') depth--;
                    pos++;
                }
                append(n->text, sizeof(n->text), src + s0, pos - 1 - s0);
            } else {
                break;
            }
        }
        if (!n->tag[0] && !n->id[0] && !n->cls[0] && !n->attrs[0] && !n->text[0]) return -1;
    }
    if (pos < end && src[pos] == '*') {
        int m = 0;
        pos++;
        while (pos < end && src[pos] >= '0' && src[pos] <= '9') m = m * 10 + (src[pos++] - '0');
        pool[id].mult = m > 0 && m <= 50 ? m : 1;
    }
    return id;
}

/* items joined by > + ^ until ')' or the end. */
static int parse_seq(int parent) {
    int cur = parent, last = parse_item(cur);
    if (last < 0) return -1;
    while (pos < end && src[pos] != ')') {
        char op = src[pos++];
        if (op == '>') {
            cur = last;
        } else if (op == '^') {
            while (pos < end && src[pos] == '^') {
                pos++;
                if (cur != parent) cur = pool[cur].parent;
            }
            if (cur != parent) cur = pool[cur].parent;
        } else if (op != '+') {
            return -1;
        }
        last = parse_item(cur);
        if (last < 0) return -1;
    }
    return 0;
}

/* ---- output -------------------------------------------------------------- */

static char *o;
static int olen, ocap, cursor_done;

static void put(const char *s, int n) {
    for (int i = 0; i < n && olen + 1 < ocap; i++) o[olen++] = s[i];
    o[olen] = '\0';
}

static void puts_(const char *s) {
    put(s, (int)str_len(s));
}

static void put_cursor(void) {
    if (!cursor_done) {
        char c = EMMET_CURSOR;
        put(&c, 1);
        cursor_done = 1;
    }
}

/* Copy s, numbering $ runs with idx (1-based) of count. */
static void put_numbered(const char *s, int idx, int empty_cursor) {
    if (!s[0] && empty_cursor) put_cursor();
    for (int i = 0; s[i];) {
        if (s[i] == '$') {
            int w = 0;
            char num[16];
            int k;
            while (s[i] == '$') w++, i++;
            fmt_u64(num, (uint64_t)idx, 10);
            for (k = (int)str_len(num); k < w; k++) put("0", 1);
            puts_(num);
        } else {
            put(s + i, 1);
            i++;
        }
    }
}

static int is_void(const char *t) {
    static const char *const v[] = { "area", "base", "br", "col", "embed", "hr", "img", "input", "link", "meta",
                                     "source", "track", "wbr", 0 };
    for (int i = 0; v[i]; i++) {
        if (str_eq(v[i], t)) return 1;
    }
    return 0;
}

static int is_inline(const char *t) {
    static const char *const v[] = { "a", "span", "em", "strong", "b", "i", "code", "label", "small", "sub", "sup",
                                     "abbr", "button", "p", "h1", "h2", "h3", "h4", "h5", "h6", 0 };
    for (int i = 0; v[i]; i++) {
        if (str_eq(v[i], t)) return 1;
    }
    return 0;
}

static const char *implicit_tag(int parent) {
    const char *p = "";
    while (parent >= 0 && pool[parent].group) parent = pool[parent].parent;
    if (parent >= 0) p = pool[parent].tag;
    if (str_eq(p, "ul") || str_eq(p, "ol")) return "li";
    if (str_eq(p, "table") || str_eq(p, "tbody") || str_eq(p, "thead") || str_eq(p, "tfoot")) return "tr";
    if (str_eq(p, "tr")) return "td";
    if (str_eq(p, "select") || str_eq(p, "datalist")) return "option";
    if (is_inline(p)) return "span";
    return "div";
}

/* Aliases and default attributes. */
static void resolve(node_t *n, char *tag, int cap, char *defattr, int dcap) {
    static const struct { const char *abbr, *tag, *attrs; } al[] = {
        { "a", "a", " href=\"\"" }, { "a:link", "a", " href=\"http://\"" }, { "a:mail", "a", " href=\"mailto:\"" },
        { "img", "img", " src=\"\" alt=\"\"" }, { "input", "input", " type=\"text\"" },
        { "input:checkbox", "input", " type=\"checkbox\"" }, { "input:c", "input", " type=\"checkbox\"" },
        { "input:radio", "input", " type=\"radio\"" }, { "input:email", "input", " type=\"email\"" },
        { "input:password", "input", " type=\"password\"" }, { "input:p", "input", " type=\"password\"" },
        { "input:submit", "input", " type=\"submit\" value=\"\"" }, { "link", "link", " rel=\"stylesheet\" href=\"\"" },
        { "link:css", "link", " rel=\"stylesheet\" href=\"style.css\"" }, { "script:src", "script", " src=\"\"" },
        { "form", "form", " action=\"\"" }, { "label", "label", " for=\"\"" }, { "btn", "button", "" },
        { "bq", "blockquote", "" }, { "hdr", "header", "" }, { "ftr", "footer", "" }, { "sect", "section", "" },
        { "art", "article", "" }, { "fig", "figure", "" }, { "iframe", "iframe", " src=\"\"" },
        { "meta:vp", "meta", " name=\"viewport\" content=\"width=device-width, initial-scale=1.0\"" },
        { "meta:utf", "meta", " charset=\"UTF-8\"" }, { "opt", "option", "" }, { "tarea", "textarea", "" },
    };
    defattr[0] = '\0';
    str_copy(tag, n->tag, cap);
    for (unsigned i = 0; i < sizeof(al) / sizeof(al[0]); i++) {
        if (str_eq(n->tag, al[i].abbr)) {
            str_copy(tag, al[i].tag, cap);
            /* explicit [attrs] win over the defaults */
            if (!n->attrs[0]) str_copy(defattr, al[i].attrs, dcap);
            return;
        }
    }
}

static void emit_attr_values(const char *attrs, int idx) {
    /* attrs is ` k="v" ...`; empty values get the cursor. */
    for (int i = 0; attrs[i];) {
        if (attrs[i] == '"' && attrs[i + 1] == '"') {
            put("\"", 1);
            put_cursor();
            put("\"", 1);
            i += 2;
        } else if (attrs[i] == '$') {
            int s = i;
            while (attrs[i] == '$') i++;
            {
                char tmp[8];
                int w = i - s;
                str_copy(tmp, "$$$$$$$", (uint64_t)w + 1);
                put_numbered(tmp, idx, 0);
            }
        } else {
            put(attrs + i, 1);
            i++;
        }
    }
}

static void emit(int id, int idx, int depth, const char *indent, const char *unit, int *first_line);

static void emit_list(int first, int idx, int depth, const char *indent, const char *unit, int *first_line) {
    for (int c = first; c >= 0; c = pool[c].next) {
        for (int k = 1; k <= pool[c].mult; k++) emit(c, pool[c].mult > 1 ? k : idx, depth, indent, unit, first_line);
    }
}

static void newline(int depth, const char *indent, const char *unit, int *first_line) {
    if (!*first_line) {
        put("\n", 1);
        puts_(indent);
        for (int d = 0; d < depth; d++) puts_(unit);
    }
    *first_line = 0;
}

static void emit(int id, int idx, int depth, const char *indent, const char *unit, int *first_line) {
    node_t *n = &pool[id];
    char tag[24], defattr[96];
    if (n->group) {
        emit_list(n->child, idx, depth, indent, unit, first_line);
        return;
    }
    if (!n->tag[0]) str_copy(n->tag, implicit_tag(n->parent), sizeof(n->tag));
    resolve(n, tag, sizeof(tag), defattr, sizeof(defattr));
    newline(depth, indent, unit, first_line);
    put("<", 1);
    puts_(tag);
    if (n->id[0]) {
        puts_(" id=\"");
        put_numbered(n->id, idx, 0);
        put("\"", 1);
    }
    if (n->cls[0]) {
        puts_(" class=\"");
        put_numbered(n->cls, idx, 0);
        put("\"", 1);
    }
    emit_attr_values(defattr, idx);
    emit_attr_values(n->attrs, idx);
    put(">", 1);
    if (is_void(tag)) return;
    if (n->child >= 0) {
        if (n->text[0]) {
            newline(depth + 1, indent, unit, first_line);
            put_numbered(n->text, idx, 0);
        }
        emit_list(n->child, idx, depth + 1, indent, unit, first_line);
        newline(depth, indent, unit, first_line);
    } else {
        put_numbered(n->text, idx, 1);
    }
    puts_("</");
    puts_(tag);
    put(">", 1);
}

static int html_boilerplate(const char *indent, const char *unit) {
    static const char *const lines[] = {
        "<!DOCTYPE html>", "<html lang=\"en\">", "<head>", "\t<meta charset=\"UTF-8\">",
        "\t<meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\">",
        "\t<title>Document</title>", "</head>", "<body>", "\t\x01", "</body>", "</html>", 0 };
    for (int i = 0; lines[i]; i++) {
        if (i) {
            put("\n", 1);
            puts_(indent);
        }
        for (const char *p = lines[i]; *p; p++) {
            if (*p == '\t') puts_(unit);
            else put(p, 1);
        }
    }
    cursor_done = 1;
    return olen;
}

/* ---- CSS ---------------------------------------------------------------- */

static const struct { const char *ab, *prop, *val; } css_tab[] = {
    { "df", "display", "flex" }, { "dg", "display", "grid" }, { "db", "display", "block" },
    { "dib", "display", "inline-block" }, { "di", "display", "inline" }, { "dn", "display", "none" },
    { "posr", "position", "relative" }, { "posa", "position", "absolute" }, { "posf", "position", "fixed" },
    { "poss", "position", "sticky" }, { "tac", "text-align", "center" }, { "tal", "text-align", "left" },
    { "tar", "text-align", "right" }, { "fwb", "font-weight", "bold" }, { "fwn", "font-weight", "normal" },
    { "jcc", "justify-content", "center" }, { "jcsb", "justify-content", "space-between" },
    { "jcsa", "justify-content", "space-around" }, { "jcfs", "justify-content", "flex-start" },
    { "jcfe", "justify-content", "flex-end" }, { "aic", "align-items", "center" },
    { "aifs", "align-items", "flex-start" }, { "aife", "align-items", "flex-end" },
    { "fdc", "flex-direction", "column" }, { "fdr", "flex-direction", "row" }, { "fww", "flex-wrap", "wrap" },
    { "ovh", "overflow", "hidden" }, { "ova", "overflow", "auto" }, { "curp", "cursor", "pointer" },
    { "tdn", "text-decoration", "none" }, { "tdu", "text-decoration", "underline" },
    { "ttu", "text-transform", "uppercase" }, { "bdn", "border", "none" }, { "fll", "float", "left" },
    { "flr", "float", "right" }, { "vh", "visibility", "hidden" }, { "boxz", "box-sizing", "border-box" },
    { "m", "margin", 0 }, { "mt", "margin-top", 0 }, { "mr", "margin-right", 0 }, { "mb", "margin-bottom", 0 },
    { "ml", "margin-left", 0 }, { "p", "padding", 0 }, { "pt", "padding-top", 0 }, { "pr", "padding-right", 0 },
    { "pb", "padding-bottom", 0 }, { "pl", "padding-left", 0 }, { "w", "width", 0 }, { "h", "height", 0 },
    { "maw", "max-width", 0 }, { "mah", "max-height", 0 }, { "miw", "min-width", 0 }, { "mih", "min-height", 0 },
    { "t", "top", 0 }, { "r", "right", 0 }, { "b", "bottom", 0 }, { "l", "left", 0 }, { "z", "z-index", 0 },
    { "fz", "font-size", 0 }, { "fw", "font-weight", 0 }, { "ff", "font-family", 0 }, { "lh", "line-height", 0 },
    { "c", "color", 0 }, { "bg", "background", 0 }, { "bgc", "background-color", 0 }, { "bd", "border", 0 },
    { "bdrs", "border-radius", 0 }, { "br", "border-radius", 0 }, { "op", "opacity", 0 },
    { "ta", "text-align", 0 }, { "td", "text-decoration", 0 }, { "ov", "overflow", 0 }, { "cur", "cursor", 0 },
    { "gap", "gap", 0 }, { "g", "gap", 0 }, { "trs", "transition", 0 }, { "trf", "transform", 0 },
    { "bxsh", "box-shadow", 0 }, { "ai", "align-items", 0 }, { "jc", "justify-content", 0 },
    { "fd", "flex-direction", 0 }, { "pos", "position", 0 }, { "d", "display", 0 }, { "fx", "flex", 0 },
    { "ac", "align-content", 0 }, { "gtc", "grid-template-columns", 0 },
};

static int unitless(const char *prop) {
    return str_eq(prop, "z-index") || str_eq(prop, "opacity") || str_eq(prop, "font-weight") ||
           str_eq(prop, "line-height") || str_eq(prop, "flex");
}

static int css_one(const char *a, int n) {
    int letters = 0, best = -1;
    while (letters < n && letters < 15 && a[letters] >= 'a' && a[letters] <= 'z') letters++;
    /* longest table entry that is a prefix of the letters */
    for (int len = letters; len > 0 && best < 0; len--) {
        for (unsigned i = 0; i < sizeof(css_tab) / sizeof(css_tab[0]); i++) {
            if ((int)str_len(css_tab[i].ab) == len) {
                int k = 0;
                while (k < len && css_tab[i].ab[k] == a[k]) k++;
                if (k == len && (len == letters || !css_tab[i].val)) {
                    best = (int)i;
                    letters = len;
                    break;
                }
            }
        }
    }
    if (best < 0) return -1;
    if (letters < n && !css_tab[best].val) {
        char c = a[letters];
        if (!((c >= '0' && c <= '9') || c == '-' || c == '.' || c == '#')) return -1;
    } else if (letters < n) {
        return -1;
    }
    puts_(css_tab[best].prop);
    puts_(": ");
    if (css_tab[best].val) {
        puts_(css_tab[best].val);
    } else if (letters == n) {
        put_cursor();
    } else if (a[letters] == '#') {
        int h = n - letters - 1;
        const char *x = a + letters + 1;
        put("#", 1);
        if (h == 1) for (int k = 0; k < 6; k++) put(x, 1);
        else if (h == 3) for (int k = 0; k < 3; k++) { put(x + k, 1); put(x + k, 1); }
        else put(x, h);
    } else {
        /* numbers joined by '-', each with an optional unit letter */
        int i = letters;
        int first = 1;
        while (i < n) {
            int neg = 0, s0;
            if (!first) put(" ", 1);
            if (a[i] == '-') {
                neg = 1;
                i++;
            }
            s0 = i;
            while (i < n && ((a[i] >= '0' && a[i] <= '9') || a[i] == '.')) i++;
            if (i == s0) return -1;
            if (neg) put("-", 1);
            if (a[s0] == '.') put("0", 1);
            put(a + s0, i - s0);
            if (i < n && a[i] != '-') {
                char u = a[i++];
                puts_(u == 'p' ? "%" : u == 'e' ? "em" : u == 'r' ? "rem" : u == 'x' ? "ex" : u == 'w' ? "vw"
                      : u == 'h' ? "vh" : "px");
                while (i < n && a[i] != '-') i++;
            } else if (!unitless(css_tab[best].prop) && !(i - s0 == 1 && a[s0] == '0')) {
                puts_("px");
            }
            if (i < n && a[i] == '-') i++;
            first = 0;
        }
    }
    put(";", 1);
    return 0;
}

/* ---- entry points ------------------------------------------------------- */

int emmet_abbr_start(int css, const char *line, int p) {
    int i = p;
    if (css) {
        while (i > 0 && line[i - 1] != ' ' && line[i - 1] != '\t' && line[i - 1] != ';' && line[i - 1] != '{') i--;
        if (i == p || !(line[i] >= 'a' && line[i] <= 'z')) return -1;
        return i;
    }
    /* HTML: back to whitespace, jumping over {text} and [attrs] */
    while (i > 0) {
        char c = line[i - 1];
        if (c == '}' || c == ']') {
            char open = c == '}' ? '{' : '[';
            int depth = 0;
            while (i > 0) {
                char d = line[--i];
                if (d == c) depth++;
                else if (d == open && --depth == 0) break;
            }
            continue;
        }
        if (c == ' ' || c == '\t') break;
        i--;
    }
    /* "...<div>ul>li": start after the last tag that closes before p */
    for (int k = p - 1; k >= i; k--) {
        if (line[k] == '<') {
            int j = k;
            while (j < p && line[j] != '>') j++;
            i = j < p ? j + 1 : p;
            break;
        }
    }
    if (i >= p) return -1;
    {
        char c = line[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '.' || c == '#' || c == '(' || c == '!' ||
              c == '[' || c == '{')) return -1;
    }
    return i;
}

int emmet_expand(int css, const char *abbr, int n, const char *indent, const char *unit, char *out, int cap) {
    o = out;
    olen = 0;
    ocap = cap;
    cursor_done = 0;
    out[0] = '\0';
    if (css) {
        int s = 0;
        for (int i = 0; i <= n; i++) {
            if (i == n || abbr[i] == '+') {
                if (s) {
                    put("\n", 1);
                    puts_(indent);
                }
                if (css_one(abbr + s, i - s) < 0) return -1;
                s = i + 1;
            }
        }
        if (!cursor_done) put_cursor();
        return olen;
    }
    if (n == 1 && abbr[0] == '!') return html_boilerplate(indent, unit);
    npool = 0;
    src = abbr;
    pos = 0;
    end = n;
    {
        int root = new_node(-1), first_line = 1;
        pool[root].group = 1;
        if (parse_seq(root) < 0 || pos != end) return -1;
        emit(root, 1, 0, indent, unit, &first_line);
    }
    if (!cursor_done) put_cursor();
    return olen;
}
