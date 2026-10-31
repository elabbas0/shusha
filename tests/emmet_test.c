/* Host tests for the Code editor's Emmet expansion and language detection:
 *   make editor-test */
#include <stdio.h>
#include <string.h>
#include "../user/apps/code_emmet.h"
#include "../user/apps/code_lang.h"

static int fails, checks;

static void expect(int css, const char *abbr, const char *want) {
    char out[4096], show[4096];
    int n = emmet_expand(css, abbr, (int)strlen(abbr), "", "  ", out, sizeof(out)), k = 0;
    checks++;
    for (int i = 0; n > 0 && out[i]; i++) show[k++] = out[i] == EMMET_CURSOR ? '|' : out[i];
    show[k] = 0;
    if (n < 0 ? want != NULL : (want == NULL || strcmp(show, want) != 0)) {
        fails++;
        printf("FAIL %s\n  got:  %s\n  want: %s\n", abbr, n < 0 ? "(none)" : show, want ? want : "(none)");
    }
}

static void detect(const char *name, const char *text, int want) {
    checks++;
    if (lang_detect(name, text, (int)strlen(text)) != want) {
        fails++;
        printf("FAIL detect %s -> %s\n", name ? name : "(no name)", lang_name(lang_detect(name, text, (int)strlen(text))));
    }
}

int main(void) {
    expect(0, "div", "<div>|</div>");
    expect(0, "ul>li.item$*3", "<ul>\n  <li class=\"item1\">|</li>\n  <li class=\"item2\"></li>\n  <li class=\"item3\"></li>\n</ul>");
    expect(0, "div#main.box.wide", "<div id=\"main\" class=\"box wide\">|</div>");
    expect(0, "a[href=#]{Go}", "<a href=\"#\">Go</a>|");
    expect(0, "a", "<a href=\"|\"></a>");
    expect(0, "img", "<img src=\"|\" alt=\"\">");
    expect(0, "h1{Title}+p", "<h1>Title</h1>\n<p>|</p>");
    expect(0, "div>(header>h1)+main", "<div>\n  <header>\n    <h1>|</h1>\n  </header>\n  <main></main>\n</div>");
    expect(0, "ul>.x", "<ul>\n  <li class=\"x\">|</li>\n</ul>");
    expect(0, "table>tr*2>td*2", "<table>\n  <tr>\n    <td>|</td>\n    <td></td>\n  </tr>\n  <tr>\n    <td></td>\n    <td></td>\n  </tr>\n</table>");
    expect(0, "div>p^span", "<div>\n  <p>|</p>\n</div>\n<span></span>");
    expect(0, "li.n$$*2", "<li class=\"n01\">|</li>\n<li class=\"n02\"></li>");
    expect(0, "input:c", "<input type=\"checkbox\">|");
    expect(0, "a:a(", NULL);
    expect(1, "m10", "margin: 10px;|");
    expect(1, "m10-20", "margin: 10px 20px;|");
    expect(1, "w100p", "width: 100%;|");
    expect(1, "df", "display: flex;|");
    expect(1, "jcsb", "justify-content: space-between;|");
    expect(1, "c#fc0", "color: #ffcc00;|");
    expect(1, "op.5", "opacity: 0.5;|");
    expect(1, "p", "padding: |;");
    expect(1, "m0+p5", "margin: 0;\npadding: 5px;|");
    expect(1, "zz", NULL);
    detect("index.html", "", L_HTML);
    detect("app.py", "", L_PY);
    detect("main.c", "", L_C);
    detect("style.css", "", L_CSS);
    detect(NULL, "#!/usr/bin/env python3\nprint(1)\n", L_PY);
    detect(NULL, "#!/bin/sh\necho hi\n", L_SH);
    detect("notes", "<!DOCTYPE html>\n<html>", L_HTML);
    detect("data", "{\n  \"a\": 1\n}", L_JSON);
    detect("x", "#include <stdio.h>\nint main(void) {}", L_C);
    detect("y", "import os\ndef f():\n  pass", L_PY);
    detect("z", "const x = () => 1;\nfunction f() {}", L_JS);
    {
        /* tokenizer spot checks */
        uint8_t cls[128];
        const char *l = "int main(void) { return 0; } // done";
        lang_line(L_C, l, (int)strlen(l), 0, cls);
        checks += 4;
        if (cls[0] != T_TYPE) fails++, printf("FAIL int is a type\n");
        if (cls[4] != T_FUNC) fails++, printf("FAIL main is a function\n");
        if (cls[17] != T_CONTROL) fails++, printf("FAIL return is control\n");
        if (cls[30] != T_COMMENT) fails++, printf("FAIL // comment\n");
        l = "<a href=\"x\">hi</a>";
        lang_line(L_HTML, l, (int)strlen(l), 0, cls);
        checks += 3;
        if (cls[1] != T_TAG || cls[3] != T_ATTR || cls[8] != T_STRING) fails++, printf("FAIL html tokens\n");
        checks++;
        if (lang_line(L_C, "/* open", 7, 0, cls) != 1) fails++, printf("FAIL block comment state\n");
    }
    printf("%d/%d editor checks passed\n", checks - fails, checks);
    return fails != 0;
}
