#ifndef SHUSHA_CODE_LANG_H
#define SHUSHA_CODE_LANG_H

/* Languages for the Code editor: recognition and syntax colouring. */

#include <stdint.h>

enum {
    L_PLAIN, L_C, L_JS, L_PY, L_HTML, L_CSS, L_JSON, L_MD, L_SH, L_COUNT
};

/* Token classes (one per character). */
enum {
    T_TEXT, T_KEYWORD, T_CONTROL, T_TYPE, T_STRING, T_NUMBER, T_COMMENT, T_FUNC, T_PUNCT,
    T_PREPROC, T_TAG, T_ATTR, T_PROP, T_VALUE, T_SELECTOR, T_HEADING, T_EMPH, T_VARIABLE, T_COUNT
};

/* Guess the language from the file name, then from the content ("syntax
 * recognition": shebangs, <!DOCTYPE, #include, JSON, def/import...). */
int         lang_detect(const char *name, const char *buf, int len);
const char *lang_name(int lang);
/* Line comment prefix, or block comment open/close for HTML/CSS. */
void        lang_comment(int lang, const char **open, const char **close);
/* Colour one line: fills cls[0..n), takes and returns the state carried
 * between lines (inside a block comment, a multi-line string, a tag...). */
int         lang_line(int lang, const char *s, int n, int state, uint8_t *cls);
/* Keywords and built-in names offered as completions (NULL-terminated). */
const char *const *lang_words(int lang);
/* Emmet works in these. */
int         lang_emmet(int lang);

#endif
