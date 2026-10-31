# Code: the editor

`user/apps/code.c` (plus `code_lang.c` and `code_emmet.c`) is modelled on
Visual Studio Code. Open it from the dock, or with "Edit" on a file in the
Workspace app.

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
  fenced code). Colours follow VS Code's Dark+ theme.
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
- **Suggestions:** Emmet, language snippets (`for`, `main`, `def`, `log`,
  `ifmain`, …), keywords, and words already in the file.
- **On screen:** bracket matching, indentation guides, current-line
  highlight, minimap, find highlights, breadcrumbs, and a status bar (Ln/Col,
  spaces, encoding, language, Emmet, read-only).
- **Undo** groups typing into words, like VS Code.

## Keys

| Keys | Action |
|---|---|
| F1 or Ctrl+Shift+P | Command palette |
| Ctrl+P | Go to file (quick open) |
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
| Ctrl+B / Ctrl+Shift+E / Ctrl+Shift+F | Toggle side bar / Explorer / Search |

The keyboard driver sends xterm-style sequences (modifier parameters, CSI
u for Ctrl+symbols), so these combinations reach the editor.

Some shortcuts differ from VS Code:
- Ctrl+H is Backspace on this keyboard, so replace is Ctrl+R.
- Ctrl+I and Ctrl+J are Tab and Enter.

## Tests

`make editor-test` runs 43 host checks: Emmet HTML/CSS expansions,
language recognition, and tokenizer spot checks.

## Not yet

- Multiple cursors.
- Mouse-wheel scrolling (the PS/2 mouse reports no wheel). Use the
  keyboard, the minimap or the scrollbar.
- A language server: suggestions are lexical, not semantic.
- Live co-editing with presence (planned; WORKSPACE.md §4).
