#ifndef SHUSHA_CODE_EMMET_H
#define SHUSHA_CODE_EMMET_H

/* Emmet abbreviations for the Code editor.
 *
 * HTML: ul>li.item$*3, div#main.box>(h1{Title}+p), a[href=#]{Go}, !, ...
 * CSS:  m10-20, w100p, df, jcsb, c#fc0, p10+m5 ...
 *
 * Expansions mark the first place the cursor should go with EMMET_CURSOR. */

#define EMMET_CURSOR '\x01'

/* Start of the abbreviation that ends at `pos` in `line`, or -1. */
int emmet_abbr_start(int css, const char *line, int pos);
/* Expand; lines after the first get `indent` in front, and `unit` is one
 * level of indentation.  Returns the length written, or -1 if `abbr` is
 * not something Emmet expands. */
int emmet_expand(int css, const char *abbr, int n, const char *indent, const char *unit, char *out, int cap);

#endif
