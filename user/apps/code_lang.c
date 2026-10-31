/* Language recognition and syntax colouring for the Code editor.
 *
 * Each tokenizer colours one line at a time and carries a small state
 * integer to the next line (block comments, multi-line strings, an open
 * CSS block, a fenced Markdown code block), so the editor only needs the
 * state at the top of the screen to colour what is visible. */
#include "lib.h"
#include "code_lang.h"

static const char *const c_kw[] = { "auto", "const", "extern", "inline", "register", "restrict", "sizeof",
    "static", "struct", "typedef", "union", "volatile", "enum", "_Alignas", "_Static_assert", "class",
    "namespace", "template", "typename", "public", "private", "protected", "virtual", "new", "delete",
    "nullptr", "true", "false", "NULL", "this", "using", 0 };
static const char *const c_ctl[] = { "if", "else", "for", "while", "do", "switch", "case", "default", "break",
    "continue", "return", "goto", "try", "catch", "throw", 0 };
static const char *const c_ty[] = { "void", "char", "short", "int", "long", "float", "double", "signed",
    "unsigned", "bool", "size_t", "uint8_t", "uint16_t", "uint32_t", "uint64_t", "int8_t", "int16_t",
    "int32_t", "int64_t", "uintptr_t", "FILE", 0 };

static const char *const js_kw[] = { "var", "let", "const", "function", "class", "extends", "new", "this",
    "super", "typeof", "instanceof", "in", "of", "async", "await", "yield", "import", "export", "from",
    "default", "static", "get", "set", "true", "false", "null", "undefined", "void", "delete",
    "interface", "type", "enum", "implements", "readonly", "public", "private", "protected", 0 };
static const char *const js_ctl[] = { "if", "else", "for", "while", "do", "switch", "case", "break",
    "continue", "return", "try", "catch", "finally", "throw", 0 };
static const char *const js_ty[] = { "Array", "Object", "String", "Number", "Boolean", "Promise", "Map", "Set",
    "Date", "JSON", "Math", "Error", "console", "document", "window", "number", "string", "boolean", "any",
    "unknown", "never", 0 };

static const char *const py_kw[] = { "def", "class", "lambda", "import", "from", "as", "global", "nonlocal",
    "and", "or", "not", "is", "in", "None", "True", "False", "async", "await", "del", "assert", "with",
    "self", 0 };
static const char *const py_ctl[] = { "if", "elif", "else", "for", "while", "return", "try", "except",
    "finally", "raise", "pass", "break", "continue", "yield", 0 };
static const char *const py_ty[] = { "int", "str", "float", "bool", "list", "dict", "set", "tuple", "bytes",
    "object", "print", "len", "range", "open", "enumerate", "zip", "map", "filter", "sorted", "isinstance",
    "super", "Exception", 0 };

static const char *const sh_kw[] = { "function", "local", "export", "readonly", "echo", "printf", "read",
    "cd", "exit", "set", "unset", "source", "test", "true", "false", "shift", "trap", 0 };
static const char *const sh_ctl[] = { "if", "then", "else", "elif", "fi", "for", "in", "do", "done", "case",
    "esac", "while", "until", "return", "break", "continue", 0 };

static const char *const html_words[] = { "html", "head", "body", "title", "meta", "link", "script", "style",
    "div", "span", "p", "a", "img", "ul", "ol", "li", "table", "thead", "tbody", "tr", "td", "th", "form",
    "input", "button", "label", "select", "option", "textarea", "header", "footer", "nav", "main",
    "section", "article", "aside", "h1", "h2", "h3", "h4", "h5", "h6", "strong", "em", "code", "pre",
    "blockquote", "figure", "figcaption", "video", "audio", "canvas", "svg", "br", "hr", "class", "id",
    "href", "src", "alt", "type", "name", "value", "placeholder", "charset", "content", "viewport", 0 };
static const char *const css_words[] = { "display", "flex", "grid", "block", "inline-block", "none",
    "position", "relative", "absolute", "fixed", "sticky", "margin", "padding", "width", "height",
    "max-width", "min-height", "color", "background", "background-color", "border", "border-radius",
    "font-size", "font-weight", "font-family", "line-height", "text-align", "center", "justify-content",
    "align-items", "flex-direction", "column", "row", "gap", "space-between", "opacity", "z-index",
    "overflow", "hidden", "auto", "transition", "transform", "box-shadow", "cursor", "pointer", 0 };
static const char *const json_words[] = { "true", "false", "null", 0 };
static const char *const md_words[] = { 0 };

const char *lang_name(int lang) {
    static const char *const names[L_COUNT] = { "Plain Text", "C", "JavaScript", "Python", "HTML", "CSS",
                                                "JSON", "Markdown", "Shell Script" };
    return lang >= 0 && lang < L_COUNT ? names[lang] : "Plain Text";
}

int lang_emmet(int lang) {
    return lang == L_HTML || lang == L_CSS;
}

const char *const *lang_words(int lang) {
    switch (lang) {
    case L_C: return c_kw;
    case L_JS: return js_kw;
    case L_PY: return py_kw;
    case L_SH: return sh_kw;
    case L_HTML: return html_words;
    case L_CSS: return css_words;
    case L_JSON: return json_words;
    default: return md_words;
    }
}

void lang_comment(int lang, const char **open, const char **close) {
    *close = 0;
    switch (lang) {
    case L_PY: case L_SH: *open = "# "; break;
    case L_HTML: case L_MD: *open = "<!-- "; *close = " -->"; break;
    case L_CSS: *open = "/* "; *close = " */"; break;
    case L_JSON: case L_PLAIN: *open = 0; break;
    default: *open = "// "; break;
    }
}

/* ---- recognition -------------------------------------------------------- */

static int ends(const char *s, const char *suffix) {
    uint64_t a = str_len(s), b = str_len(suffix);
    if (a < b) return 0;
    for (uint64_t i = 0; i < b; i++) {
        char x = s[a - b + i];
        if (x >= 'A' && x <= 'Z') x = (char)(x + 32);
        if (x != suffix[i]) return 0;
    }
    return 1;
}

static int has(const char *buf, int len, const char *needle) {
    int n = (int)str_len(needle);
    for (int i = 0; i + n <= len && i < 4096; i++) {
        int k = 0;
        while (k < n && buf[i + k] == needle[k]) k++;
        if (k == n) return 1;
    }
    return 0;
}

int lang_detect(const char *name, const char *buf, int len) {
    static const struct { const char *ext; int lang; } map[] = {
        { ".c", L_C }, { ".h", L_C }, { ".cpp", L_C }, { ".hpp", L_C }, { ".cc", L_C },
        { ".js", L_JS }, { ".mjs", L_JS }, { ".ts", L_JS }, { ".jsx", L_JS }, { ".tsx", L_JS },
        { ".py", L_PY }, { ".html", L_HTML }, { ".htm", L_HTML }, { ".css", L_CSS }, { ".json", L_JSON },
        { ".md", L_MD }, { ".markdown", L_MD }, { ".sh", L_SH }, { ".bash", L_SH }, { ".txt", L_PLAIN },
    };
    int i = 0;
    for (unsigned k = 0; name && k < sizeof(map) / sizeof(map[0]); k++) {
        if (ends(name, map[k].ext)) return map[k].lang;
    }
    /* No (known) extension: look at the content. */
    while (i < len && (buf[i] == ' ' || buf[i] == '\n' || buf[i] == '\t' || buf[i] == '\r')) i++;
    if (len - i > 2 && buf[i] == '#' && buf[i + 1] == '!') {
        if (has(buf + i, len - i, "python")) return L_PY;
        if (has(buf + i, len - i, "node")) return L_JS;
        return L_SH;
    }
    if (has(buf, len, "<!DOCTYPE") || has(buf, len, "<!doctype") || has(buf, len, "<html") ||
        has(buf, len, "<div")) return L_HTML;
    if (i < len && (buf[i] == '{' || buf[i] == '[') && has(buf, len, "\":")) return L_JSON;
    if (has(buf, len, "#include") || has(buf, len, "int main(")) return L_C;
    if (has(buf, len, "def ") || has(buf, len, "import ") || has(buf, len, "elif ")) return L_PY;
    if (has(buf, len, "function ") || has(buf, len, "const ") || has(buf, len, "=> ")) return L_JS;
    if (i < len && buf[i] == '#' && i + 1 < len && buf[i + 1] == ' ') return L_MD;
    if (has(buf, len, "{") && has(buf, len, ";") && has(buf, len, ":") && !has(buf, len, "(")) return L_CSS;
    return L_PLAIN;
}

/* ---- helpers ------------------------------------------------------------ */

static int is_id0(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '$';
}

static int is_id(char c) {
    return is_id0(c) || (c >= '0' && c <= '9');
}

static int is_digit(char c) {
    return c >= '0' && c <= '9';
}

static int in_list(const char *const *list, const char *s, int n) {
    for (int i = 0; list && list[i]; i++) {
        int k = 0;
        while (k < n && list[i][k] == s[k]) k++;
        if (k == n && list[i][n] == '\0') return 1;
    }
    return 0;
}

static void fill(uint8_t *cls, int from, int to, int t) {
    for (int i = from; i < to; i++) cls[i] = (uint8_t)t;
}

/* A quoted string from s[i] (the quote); returns the index after it. */
static int string_end(const char *s, int n, int i) {
    char q = s[i++];
    while (i < n && s[i] != q) i += s[i] == '\\' ? 2 : 1;
    return i < n ? i + 1 : n;
}

static int number_end(const char *s, int n, int i) {
    while (i < n && (is_id(s[i]) || s[i] == '.')) i++;
    return i;
}

/* ---- C, JavaScript, Python, shell -------------------------------------- */

/* state: 1 = inside a block comment / triple-quoted string / template */
static int code_line(int lang, const char *s, int n, int state, uint8_t *cls) {
    const char *const *kw = lang == L_C ? c_kw : lang == L_JS ? js_kw : lang == L_PY ? py_kw : sh_kw;
    const char *const *ctl = lang == L_C ? c_ctl : lang == L_JS ? js_ctl : lang == L_PY ? py_ctl : sh_ctl;
    const char *const *ty = lang == L_C ? c_ty : lang == L_JS ? js_ty : lang == L_PY ? py_ty : 0;
    int i = 0, first = 1, prev_def = 0;
    fill(cls, 0, n, T_TEXT);
    while (i < n) {
        char c = s[i];
        if (state == 1) { /* continuing a block comment / docstring / template */
            int j = i;
            if (lang == L_PY) {
                while (j + 2 < n && !(s[j] == '"' && s[j + 1] == '"' && s[j + 2] == '"')) j++;
                if (j + 2 < n) { fill(cls, i, j + 3, T_STRING); i = j + 3; state = 0; }
                else { fill(cls, i, n, T_STRING); return 1; }
            } else {
                while (j + 1 < n && !(s[j] == '*' && s[j + 1] == '/')) j++;
                if (j + 1 < n) { fill(cls, i, j + 2, T_COMMENT); i = j + 2; state = 0; }
                else { fill(cls, i, n, T_COMMENT); return 1; }
            }
            continue;
        }
        if (c == ' ' || c == '\t') { i++; continue; }
        if ((lang == L_C || lang == L_JS) && c == '/' && i + 1 < n && s[i + 1] == '/') {
            fill(cls, i, n, T_COMMENT);
            return 0;
        }
        if ((lang == L_C || lang == L_JS) && c == '/' && i + 1 < n && s[i + 1] == '*') {
            fill(cls, i, i + 2, T_COMMENT);
            i += 2;
            state = 1;
            continue;
        }
        if ((lang == L_PY || lang == L_SH) && c == '#' && !(lang == L_SH && i && s[i - 1] == '$')) {
            fill(cls, i, n, T_COMMENT);
            return 0;
        }
        if (lang == L_C && c == '#' && first) {
            int j = i + 1;
            while (j < n && is_id(s[j])) j++;
            fill(cls, i, j, T_PREPROC);
            i = j;
            first = 0;
            continue;
        }
        if (lang == L_PY && c == '"' && i + 2 < n && s[i + 1] == '"' && s[i + 2] == '"') {
            int j = i + 3;
            while (j + 2 < n && !(s[j] == '"' && s[j + 1] == '"' && s[j + 2] == '"')) j++;
            if (j + 2 < n) { fill(cls, i, j + 3, T_STRING); i = j + 3; continue; }
            fill(cls, i, n, T_STRING);
            return 1;
        }
        if (c == '"' || c == '\'' || (lang == L_JS && c == '`')) {
            int j = string_end(s, n, i);
            fill(cls, i, j, T_STRING);
            /* #include <file> / "file" */
            i = j;
            first = 0;
            continue;
        }
        if (lang == L_C && c == '<' && i > 0 && cls[0] == T_PREPROC) {
            int j = i;
            while (j < n && s[j] != '>') j++;
            fill(cls, i, j < n ? j + 1 : n, T_STRING);
            i = j < n ? j + 1 : n;
            continue;
        }
        if (lang == L_SH && c == '$') {
            int j = i + 1;
            if (j < n && s[j] == '{') { while (j < n && s[j] != '}') j++; j++; }
            else while (j < n && (is_id(s[j]) || s[j] == '#' || s[j] == '@' || s[j] == '?')) j++;
            fill(cls, i, j > n ? n : j, T_VARIABLE);
            i = j > n ? n : j;
            continue;
        }
        if (lang == L_PY && c == '@' && first) {
            int j = i + 1;
            while (j < n && (is_id(s[j]) || s[j] == '.')) j++;
            fill(cls, i, j, T_FUNC);
            i = j;
            continue;
        }
        if (is_digit(c) || (c == '.' && i + 1 < n && is_digit(s[i + 1]))) {
            int j = number_end(s, n, i);
            if (!(i && is_id(s[i - 1]))) fill(cls, i, j, T_NUMBER);
            i = j;
            first = 0;
            continue;
        }
        if (is_id0(c) || (lang == L_SH && c == '-')) {
            int j = i, k;
            while (j < n && (is_id(s[j]) || (lang == L_SH && s[j] == '-'))) j++;
            k = j;
            while (k < n && s[k] == ' ') k++;
            if (in_list(ctl, s + i, j - i)) fill(cls, i, j, T_CONTROL);
            else if (in_list(kw, s + i, j - i)) fill(cls, i, j, T_KEYWORD);
            else if (prev_def) fill(cls, i, j, prev_def == 2 ? T_TYPE : T_FUNC);
            else if (k < n && s[k] == '(') fill(cls, i, j, T_FUNC);
            else if (ty && in_list(ty, s + i, j - i)) fill(cls, i, j, T_TYPE);
            else if (lang == L_JS && c >= 'A' && c <= 'Z') fill(cls, i, j, T_TYPE);
            else if (lang == L_JS || lang == L_PY) fill(cls, i, j, T_VARIABLE);
            prev_def = (lang == L_PY && (j - i == 3 && s[i] == 'd' && s[i + 1] == 'e' && s[i + 2] == 'f')) ? 1
                     : (j - i == 5 && s[i] == 'c' && s[i + 1] == 'l' && s[i + 2] == 'a') ? 2
                     : (lang == L_JS && j - i == 8 && s[i] == 'f' && s[i + 1] == 'u') ? 1 : 0;
            i = j;
            first = 0;
            continue;
        }
        cls[i] = T_PUNCT;
        i++;
        first = 0;
    }
    return state;
}

/* ---- JSON ---------------------------------------------------------------- */

static int json_line(const char *s, int n, uint8_t *cls) {
    int i = 0;
    fill(cls, 0, n, T_TEXT);
    while (i < n) {
        char c = s[i];
        if (c == '"') {
            int j = string_end(s, n, i), k = j;
            while (k < n && s[k] == ' ') k++;
            fill(cls, i, j, k < n && s[k] == ':' ? T_PROP : T_STRING);
            i = j;
        } else if (is_digit(c) || c == '-') {
            int j = number_end(s, n, i + 1);
            fill(cls, i, j, T_NUMBER);
            i = j;
        } else if (is_id0(c)) {
            int j = i;
            while (j < n && is_id(s[j])) j++;
            fill(cls, i, j, T_KEYWORD);
            i = j;
        } else {
            if (c != ' ') cls[i] = T_PUNCT;
            i++;
        }
    }
    return 0;
}

/* ---- CSS ----------------------------------------------------------------- */

/* state bits: 1 inside { }, 2 inside a comment */
static int css_line(const char *s, int n, int state, uint8_t *cls) {
    int i = 0, value = 0;
    fill(cls, 0, n, T_TEXT);
    while (i < n) {
        char c = s[i];
        if (state & 2) {
            int j = i;
            while (j + 1 < n && !(s[j] == '*' && s[j + 1] == '/')) j++;
            if (j + 1 < n) { fill(cls, i, j + 2, T_COMMENT); i = j + 2; state &= ~2; }
            else { fill(cls, i, n, T_COMMENT); return state; }
            continue;
        }
        if (c == '/' && i + 1 < n && s[i + 1] == '*') { state |= 2; continue; }
        if (c == ' ' || c == '\t') { i++; continue; }
        if (c == '{') { cls[i++] = T_PUNCT; state |= 1; value = 0; continue; }
        if (c == '}') { cls[i++] = T_PUNCT; state &= ~1; value = 0; continue; }
        if (!(state & 1)) { /* selectors */
            int j = i;
            if (c == '@') {
                while (j < n && s[j] != ' ' && s[j] != '{') j++;
                fill(cls, i, j, T_CONTROL);
                i = j;
                continue;
            }
            while (j < n && s[j] != '{' && s[j] != ',' && s[j] != ' ') j++;
            fill(cls, i, j, T_SELECTOR);
            i = j > i ? j : i + 1;
            continue;
        }
        if (c == ':' && !value) { cls[i++] = T_PUNCT; value = 1; continue; }
        if (c == ';') { cls[i++] = T_PUNCT; value = 0; continue; }
        if (c == '"' || c == '\'') {
            int j = string_end(s, n, i);
            fill(cls, i, j, T_STRING);
            i = j;
            continue;
        }
        if (value && (is_digit(c) || c == '#' || (c == '.' && i + 1 < n && is_digit(s[i + 1])) ||
                      (c == '-' && i + 1 < n && is_digit(s[i + 1])))) {
            int j = number_end(s, n, i + 1);
            while (j < n && s[j] == '%') j++;
            fill(cls, i, j, T_NUMBER);
            i = j;
            continue;
        }
        if (is_id0(c) || c == '-') {
            int j = i;
            while (j < n && (is_id(s[j]) || s[j] == '-')) j++;
            fill(cls, i, j, value ? T_VALUE : T_PROP);
            i = j;
            continue;
        }
        cls[i++] = T_PUNCT;
    }
    return state;
}

/* ---- HTML ---------------------------------------------------------------- */

/* state: 0 text, 1 inside a tag, 2 inside <!-- --> */
static int html_line(const char *s, int n, int state, uint8_t *cls) {
    int i = 0;
    fill(cls, 0, n, T_TEXT);
    while (i < n) {
        char c = s[i];
        if (state == 2) {
            int j = i;
            while (j + 2 < n && !(s[j] == '-' && s[j + 1] == '-' && s[j + 2] == '>')) j++;
            if (j + 2 < n) { fill(cls, i, j + 3, T_COMMENT); i = j + 3; state = 0; }
            else { fill(cls, i, n, T_COMMENT); return 2; }
            continue;
        }
        if (state == 0) {
            if (c == '<' && i + 3 < n && s[i + 1] == '!' && s[i + 2] == '-' && s[i + 3] == '-') {
                state = 2;
                continue;
            }
            if (c == '<' && i + 1 < n && (is_id0(s[i + 1]) || s[i + 1] == '/' || s[i + 1] == '!')) {
                int j = i + 1;
                if (s[j] == '/' || s[j] == '!') j++;
                while (j < n && (is_id(s[j]) || s[j] == '-')) j++;
                fill(cls, i, i + 1, T_PUNCT);
                fill(cls, i + 1, j, s[i + 1] == '!' ? T_KEYWORD : T_TAG);
                i = j;
                state = 1;
                continue;
            }
            if (c == '&') {
                int j = i + 1;
                while (j < n && j - i < 10 && s[j] != ';' && s[j] != ' ') j++;
                if (j < n && s[j] == ';') {
                    fill(cls, i, j + 1, T_KEYWORD);
                    i = j + 1;
                    continue;
                }
            }
            i++;
            continue;
        }
        /* inside a tag */
        if (c == '>' || (c == '/' && i + 1 < n && s[i + 1] == '>')) {
            int j = c == '>' ? i + 1 : i + 2;
            fill(cls, i, j, T_PUNCT);
            i = j;
            state = 0;
            continue;
        }
        if (c == '"' || c == '\'') {
            int j = string_end(s, n, i);
            fill(cls, i, j, T_STRING);
            i = j;
            continue;
        }
        if (is_id0(c) || c == '-' || c == ':' || c == '@') {
            int j = i;
            while (j < n && (is_id(s[j]) || s[j] == '-' || s[j] == ':' || s[j] == '@')) j++;
            fill(cls, i, j, T_ATTR);
            i = j;
            continue;
        }
        if (c != ' ') cls[i] = T_PUNCT;
        i++;
    }
    return state;
}

/* ---- Markdown -------------------------------------------------------------- */

/* state: 1 inside a ``` fence */
static int md_line(const char *s, int n, int state, uint8_t *cls) {
    int i = 0;
    fill(cls, 0, n, T_TEXT);
    if (n >= 3 && s[0] == '`' && s[1] == '`' && s[2] == '`') {
        fill(cls, 0, n, T_STRING);
        return !state;
    }
    if (state) {
        fill(cls, 0, n, T_STRING);
        return 1;
    }
    while (i < n && s[i] == ' ') i++;
    if (i < n && s[i] == '#') {
        fill(cls, 0, n, T_HEADING);
        return 0;
    }
    if (i < n && s[i] == '>') {
        fill(cls, 0, n, T_COMMENT);
        return 0;
    }
    if (i + 1 < n && (s[i] == '-' || s[i] == '*' || s[i] == '+') && s[i + 1] == ' ') cls[i] = T_KEYWORD;
    while (i < n) {
        if (s[i] == '`') {
            int j = i + 1;
            while (j < n && s[j] != '`') j++;
            fill(cls, i, j < n ? j + 1 : n, T_STRING);
            i = j + 1;
        } else if (s[i] == '*' || s[i] == '_') {
            char m = s[i];
            int j = i + 1;
            while (j < n && s[j] != m) j++;
            if (j < n && j > i + 1) {
                while (j + 1 < n && s[j + 1] == m) j++;
                fill(cls, i, j + 1, T_EMPH);
                i = j + 1;
            } else {
                i++;
            }
        } else if (s[i] == '[') {
            int j = i;
            while (j < n && s[j] != ']') j++;
            fill(cls, i, j < n ? j + 1 : n, T_FUNC);
            i = j + 1;
            if (i < n && s[i] == '(') {
                int k = i;
                while (k < n && s[k] != ')') k++;
                fill(cls, i, k < n ? k + 1 : n, T_STRING);
                i = k + 1;
            }
        } else {
            i++;
        }
    }
    return 0;
}

int lang_line(int lang, const char *s, int n, int state, uint8_t *cls) {
    switch (lang) {
    case L_C: case L_JS: case L_PY: case L_SH: return code_line(lang, s, n, state, cls);
    case L_JSON: return json_line(s, n, cls);
    case L_CSS: return css_line(s, n, state, cls);
    case L_HTML: return html_line(s, n, state, cls);
    case L_MD: return md_line(s, n, state, cls);
    default:
        fill(cls, 0, n, T_TEXT);
        return 0;
    }
}

/* ---- outline -------------------------------------------------------------- */

static int word_at(const char *s, int n, int i, char *out, int cap) {
    int k = 0;
    while (i < n && (s[i] == ' ' || s[i] == '\t')) i++;
    while (i < n && (is_id(s[i]) || s[i] == '.' || s[i] == '-') && k < cap - 1) out[k++] = s[i++];
    out[k] = '\0';
    return k;
}

static int starts(const char *s, int n, int i, const char *w) {
    int k = 0;
    while (w[k] && i + k < n && s[i + k] == w[k]) k++;
    return !w[k] && (i + k >= n || !is_id(s[i + k]));
}

static int copy_trim(char *out, int cap, const char *s, int n) {
    int a = 0, k = 0;
    while (a < n && (s[a] == ' ' || s[a] == '\t')) a++;
    while (n > a && (s[n - 1] == ' ' || s[n - 1] == '{' || s[n - 1] == '\r')) n--;
    for (int i = a; i < n && k < cap - 1; i++) out[k++] = s[i];
    out[k] = '\0';
    return k;
}

int lang_symbol(int lang, const char *s, int n, char *label, int cap) {
    int i = 0;
    while (i < n && (s[i] == ' ' || s[i] == '\t')) i++;
    if (i >= n) return 0;
    switch (lang) {
    case L_PY:
        if (starts(s, n, i, "def") && word_at(s, n, i + 3, label, cap)) return SYM_FUNC;
        if (starts(s, n, i, "async") && starts(s, n, i + 6, "def") && word_at(s, n, i + 9, label, cap)) return SYM_FUNC;
        if (starts(s, n, i, "class") && word_at(s, n, i + 5, label, cap)) return SYM_CLASS;
        return 0;
    case L_JS:
        if (starts(s, n, i, "export")) i += 6;
        if (starts(s, n, i, " default")) i += 8;
        while (i < n && s[i] == ' ') i++;
        if (starts(s, n, i, "async")) i += 6;
        while (i < n && s[i] == ' ') i++;
        if (starts(s, n, i, "function") && word_at(s, n, i + 8, label, cap)) return SYM_FUNC;
        if (starts(s, n, i, "class") && word_at(s, n, i + 5, label, cap)) return SYM_CLASS;
        if ((starts(s, n, i, "const") || starts(s, n, i, "let")) && word_at(s, n, i + (s[i] == 'c' ? 5 : 3), label, cap)) {
            for (int k = i; k + 1 < n; k++) {
                if ((s[k] == '=' && s[k + 1] == '>') || starts(s, n, k, "function")) return SYM_FUNC;
            }
        }
        return 0;
    case L_C: {
        /* "type name(args)" at the start of a line, not a call or prototype */
        int open = -1, k;
        if (s[i] == '#') {
            if (starts(s, n, i + 1, "define") && word_at(s, n, i + 7, label, cap)) return SYM_MACRO;
            return 0;
        }
        if (i > 0 || starts(s, n, 0, "return") || starts(s, n, 0, "if") || starts(s, n, 0, "else")) return 0;
        if ((starts(s, n, 0, "struct") || starts(s, n, 0, "typedef")) && s[n - 1] == '{') {
            copy_trim(label, cap, s, n);
            return SYM_CLASS;
        }
        for (k = 0; k < n; k++) {
            if (s[k] == '(') {
                open = k;
                break;
            }
            if (s[k] == '=' || s[k] == ';') return 0;
        }
        if (open <= 0 || s[n - 1] == ';') return 0;
        k = open;
        while (k > 0 && s[k - 1] == ' ') k--;
        {
            int e = k;
            while (k > 0 && is_id(s[k - 1])) k--;
            if (e == k || k == 0) return 0; /* needs a return type in front */
            copy_trim(label, cap, s + k, e - k);
        }
        return SYM_FUNC;
    }
    case L_SH: {
        int k = i;
        if (starts(s, n, i, "function") && word_at(s, n, i + 8, label, cap)) return SYM_FUNC;
        while (k < n && (is_id(s[k]) || s[k] == '-')) k++;
        if (k > i && k + 1 < n && s[k] == '(' && s[k + 1] == ')') {
            copy_trim(label, cap, s + i, k - i);
            return SYM_FUNC;
        }
        return 0;
    }
    case L_MD:
        if (s[i] == '#') {
            int depth = 0;
            while (i < n && s[i] == '#') i++, depth++;
            copy_trim(label, cap, s + i, n - i);
            return depth <= 3 && label[0] ? SYM_HEADING : 0;
        }
        return 0;
    case L_CSS:
        if (s[n - 1] == '{' || (n > 1 && s[n - 1] == ' ' && s[n - 2] == '{')) {
            copy_trim(label, cap, s, n);
            return label[0] ? SYM_RULE : 0;
        }
        return 0;
    case L_JSON:
        if (i > 0 && i <= 4 && s[i] == '"') {
            int k = i + 1, o = 0;
            while (k < n && s[k] != '"' && o < cap - 1) label[o++] = s[k++];
            label[o] = '\0';
            return o ? SYM_KEY : 0;
        }
        return 0;
    case L_HTML: {
        /* headings with their text, landmarks, and elements with an id */
        static const char *const marks[] = { "header", "nav", "main", "section", "article", "aside", "footer",
                                             "form", "table", 0 };
        char tag[16];
        int k;
        if (s[i] != '<' || i + 1 >= n || !is_id0(s[i + 1])) return 0;
        word_at(s, n, i + 1, tag, sizeof(tag));
        if (tag[0] == 'h' && tag[1] >= '1' && tag[1] <= '3' && !tag[2]) {
            int a = i, b;
            while (a < n && s[a] != '>') a++;
            b = a + 1;
            while (b < n && s[b] != '<') b++;
            if (a < n && b > a + 1) {
                copy_trim(label, cap, s + a + 1, b - a - 1);
                return SYM_HEADING;
            }
        }
        for (k = i; k + 3 < n && s[k] != '>'; k++) {
            if (s[k] == 'i' && s[k + 1] == 'd' && s[k + 2] == '=' && s[k + 3] == '"') {
                int e = k + 4, o;
                str_copy(label, tag, (uint64_t)cap);
                o = (int)str_len(label);
                if (o < cap - 1) label[o++] = '#';
                while (e < n && s[e] != '"' && o < cap - 1) label[o++] = s[e++];
                label[o] = '\0';
                return SYM_TAG;
            }
        }
        for (k = 0; marks[k]; k++) {
            if (str_eq(tag, marks[k])) {
                str_copy(label, tag, (uint64_t)cap);
                return SYM_TAG;
            }
        }
        return 0;
    }
    }
    return 0;
}
