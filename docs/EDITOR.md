# Qələm: the editor

Qələm ("pen" in Azerbaijani) is Shusha's code and text editor:
`user/apps/code.c`, plus `code_lang.c` and `code_emmet.c`. Open it from the
dock (the gold nib), or with "Edit" on a file in the Workspace app.

## Design

Qələm has its own look, built from Shusha's visual language rather than
copied from another editor.

- **Library (left).**
  - The **workspace** lists every shared file with a language badge. Each
    file shows its owner as an avatar coloured by rank. Files you cannot
    open show a padlock, and their names are hidden.
  - **Home** holds your private files.
  - A note at the bottom says workspace files are decrypted in memory only.
- **Header.** Open documents are rounded pills with two-letter language
  badges and a gold dot for unsaved changes. "Find a file or action" sits
  at the right.
- **The document card.**
  - Its header shows where the file lives (`shusha-team / index.html` with
    a padlock, or `~ / notes.md`) and has Find, Outline and Save buttons.
  - The text has a gold bar on the current line, gold underlines for
    matching brackets, dotted indentation guides, and amber find matches.
  - Footer capsules show the location ("encrypted · shusha-team" in plum,
    or "private · home" in mint) and Ln/Col, language, Emmet and
    indentation.
  - Messages appear as toasts above the footer.
- **Outline (right).** Functions, classes, headings, landmarks, CSS rules,
  JSON keys and macros of the document. The symbol you are in is marked;
  clicking one jumps to it. The thin overview ribbon beside the text shows
  where you are and where the find matches are.
- **The finder (F1 / Ctrl+P).** A centred spotlight over a dimmed window.
  Results are grouped as Actions and Files, with shortcuts drawn as
  keycaps.
- **Colours: "Şuşa night."**
  - magenta keywords and tags;
  - rose control flow;
  - gold functions and attributes;
  - green strings;
  - amber numbers;
  - teal types and properties.

  All taken from the Xarıbülbül emblem and Shusha's accent.

## Files

- **The Explorer sidebar** lists the workspace's shared files and the
  files in your home.
  - Files you cannot open show a padlock. Their names are hidden from you
    too.
- **Workspace files** are decrypted by `workspaced` into the editor's
  memory only.
- **Ctrl+S on a workspace file** sends the new text back. There it is
  re-encrypted under the file's key, given the next version number, signed
  by you, and replicated to every member.
- **Who can edit.** The owner and higher ranks can edit. A lower rank who
  was granted access gets a read-only editor.
- **Saving a new file.** "Workspace: Save as Encrypted Workspace File"
  shares an untitled or home file as a new workspace file that you own.
- **Home files** save with your own permissions. New ones are private
  (0600).

## Language recognition and colouring

- **By file name:** `.c .h .cpp .js .ts .jsx .py .html .css .json .md .sh`.
- **Otherwise by content:** shebang lines, `<!DOCTYPE`/`<html`, JSON
  objects, `#include`, `def`/`import`, `function`/`=>`, Markdown headings,
  and CSS rules.
- **Colouring:** each language has its own tokenizer, which carries state
  between lines (block comments, docstrings, open CSS blocks, HTML tags,
  fenced code). Colours are "Şuşa night" (see Design).
- **Changing it:** click the language in the status bar, or run "Change
  Language Mode".

## Emmet (HTML and CSS)

Type an abbreviation and press Tab. As you type, the suggestion widget
shows "Emmet Abbreviation" with a preview of the result.

- **HTML:**
  - tags, `#id`, `.class`, `[attr=value]`, `{text}`;
  - `*N` repeats, and `$` / `$$` number the copies;
  - `>` child, `+` sibling, `^` climb up, `( )` groups;
  - implicit tags: `ul>.x` gives `li`, `tr>.c` gives `td`, otherwise `div`
    or `span`;
  - aliases (`btn`, `bq`, `input:c`, `link:css`, …) with default
    attributes;
  - `!` gives the HTML5 boilerplate.
- **CSS:** `m10-20`, `w100p`, `p.5e`, `df`, `jcsb`, `aic`, `posa`,
  `c#fc0`, `bgc#1e1e1e`, `bdrs8`, … Join several with `+`.
- **The cursor** lands on the first empty attribute or element, as in VS
  Code.

## Editing

- **Brackets and quotes:** they auto-close, you can type over the closer,
  and they wrap a selection. HTML tags auto-close as you finish the
  opening tag.
- **Enter** keeps the indentation and adds a level after `{ [ (`, a
  Python `:` or an HTML tag. Between a pair it splits onto three lines.
- **Backspace** deletes a whole indentation step, or an empty pair.
- **Suggestions:** Emmet (only for real tags or abbreviations with
  operators, so plain words in text stay plain), language snippets (`for`, `main`, `def`, `log`,
  `ifmain`, …), keywords, and words already in the file.
- **On screen:** bracket matching, indentation guides, the current line,
  the outline, the overview ribbon, find highlights and footer capsules (see
  Design).
- **Undo** groups typing into words.

## Keys

The shortcuts are the ones most code editors share, so they need no
relearning.

| Keys | Action |
|---|---|
| F1 or Ctrl+Shift+P | The finder: actions and files |
| Ctrl+P | The finder: files first |
| Ctrl+N / Ctrl+S / Ctrl+W | New / save / close |
| Ctrl+Z / Ctrl+Y (Ctrl+Shift+Z) | Undo / redo |
| Ctrl+F / Ctrl+R / F3, Shift+F3 | Find / replace / next, previous |
| Ctrl+G | Go to line |
| Ctrl+Space | Trigger suggest |
| Ctrl+/ | Toggle line comment |
| Ctrl+D / Ctrl+L / Ctrl+A | Select word / line / all |
| Ctrl+X / Ctrl+C / Ctrl+V | Cut / copy / paste (the whole line if nothing is selected) |
| Ctrl+Shift+K | Delete line |
| Alt+Up / Alt+Down | Move line |
| Shift+Alt+Down | Duplicate line |
| Tab / Shift+Tab | Indent / outdent (Tab also expands Emmet) |
| Ctrl+Left / Ctrl+Right | Move by word (with Shift: select) |
| Home / End / Ctrl+Home / Ctrl+End / PgUp / PgDn | Navigate (Home is smart) |
| Ctrl+PgUp / Ctrl+PgDn | Previous / next tab |
| Ctrl+B / Ctrl+Shift+O | Show or hide the library / the outline |

The keyboard driver sends xterm-style sequences (modifier parameters, CSI
u for Ctrl+symbols), so these combinations reach the editor.

A few keys cannot be bound:
- Ctrl+H is Backspace on this keyboard, so replace is Ctrl+R.
- Ctrl+I and Ctrl+J are Tab and Enter.

## Tests

`make editor-test` runs 54 host checks: Emmet HTML/CSS expansions,
language recognition, tokenizer spot checks and outline symbols.

## Not yet

- Multiple cursors.
- Mouse-wheel scrolling (the PS/2 mouse reports no wheel). Use the
  keyboard, the outline or the overview ribbon.
- A language server: suggestions are lexical, not semantic.
- Live co-editing with presence (planned; WORKSPACE.md §4).
