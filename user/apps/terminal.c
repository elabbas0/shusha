/* Shusha Terminal.  Ported from the ICDA terminal app onto the Shusha
 * window system: the grid is sized to the window, only changed rows are
 * redrawn and presented, and it shares the shell's built-in commands. */
#include "lib.h"
#include "ui.h"

#define COLS 88
#define ROWS 26
#define PAD  8
#define WIN_W (COLS * GLYPH_W + 2 * PAD)
#define WIN_H (ROWS * GLYPH_H + 2 * PAD)

#define FG      COL_TEXT
#define FG_DIM  COL_MUTED
#define FG_OK   COL_ACCENT_HI
#define FG_ERR  COL_DANGER
#define BG      0x0b1017u

static char cells[ROWS][COLS];
static uint32_t colors[ROWS][COLS];
static uint8_t dirty[ROWS];
static int cur_r, cur_c;
static char line[160];
static int line_len;
static char io[8192];
static int esc_state;
static int full_redraw = 1;

static int ctx_open, ctx_x, ctx_y, mouse_x, mouse_y;
static uint32_t prev_buttons;
static const menu_t ctx_menu = { { "Clear", "New Terminal", "Close" }, 3 };

static void mark_all(void) {
    for (int r = 0; r < ROWS; r++) dirty[r] = 1;
}

static void clear_screen(void) {
    for (int r = 0; r < ROWS; r++) {
        for (int c = 0; c < COLS; c++) {
            cells[r][c] = ' ';
            colors[r][c] = FG;
        }
    }
    cur_r = cur_c = 0;
    mark_all();
}

static void scroll(void) {
    for (int r = 0; r < ROWS - 1; r++) {
        mem_copy(cells[r], cells[r + 1], COLS);
        mem_copy(colors[r], colors[r + 1], sizeof(colors[r]));
    }
    for (int c = 0; c < COLS; c++) {
        cells[ROWS - 1][c] = ' ';
        colors[ROWS - 1][c] = FG;
    }
    cur_r = ROWS - 1;
    mark_all();
}

static void putc_col(char ch, uint32_t col) {
    dirty[cur_r] = 1;
    if (ch == '\n') {
        cur_c = 0;
        if (++cur_r >= ROWS) scroll();
        return;
    }
    if (ch == '\b') {
        if (cur_c > 0) cells[cur_r][--cur_c] = ' ';
        return;
    }
    if (ch == '\t') ch = ' ';
    if ((unsigned char)ch < 32 || (unsigned char)ch > 126) return;
    cells[cur_r][cur_c] = ch;
    colors[cur_r][cur_c] = col;
    if (++cur_c >= COLS) {
        cur_c = 0;
        if (++cur_r >= ROWS) scroll();
    }
    dirty[cur_r] = 1;
}

static void out(const char *s, uint32_t col) {
    while (*s) putc_col(*s++, col);
}

static void prompt(void) {
    char cwd[128];
    if (sh_getcwd(cwd, sizeof(cwd)) < 0) str_copy(cwd, "?", sizeof(cwd));
    out("shusha", FG_OK);
    out(":", FG_DIM);
    out(cwd, 0x7aa2f7);
    out("$ ", FG_DIM);
}

/* ---- commands ---------------------------------------------------------- */

static void err(const char *msg) {
    out(msg, FG_ERR);
    out("\n", FG);
}

static void join_args(char *dst, uint64_t cap, int argc, char **argv, int from) {
    dst[0] = '\0';
    for (int i = from; i < argc; i++) {
        if (i > from) str_cat(dst, " ", cap);
        str_cat(dst, argv[i], cap);
    }
}

static void run_program(const char *path, const char *args) {
    long pid = args && args[0] ? sh_spawn_args(path, args) : sh_spawn(path);
    if (pid < 0) err("cannot start program");
    else out("started\n", FG_DIM);
}

static void execute(char *cmdline) {
    char *argv[16], buf[256];
    int argc = split_args(cmdline, argv, 16);
    const char *c;
    sh_stat_t st;

    if (!argc) return;
    c = argv[0];
    if (str_eq(c, "help")) {
        out("help clear pwd cd ls cat echo mkdir touch write stat ps run random about exit\n"
            "any other word runs /apps/<word>.app (or a path)\n", FG_DIM);
    } else if (str_eq(c, "clear")) {
        clear_screen();
    } else if (str_eq(c, "pwd")) {
        if (sh_getcwd(buf, sizeof(buf)) >= 0) {
            out(buf, FG);
            out("\n", FG);
        }
    } else if (str_eq(c, "cd")) {
        if (sh_chdir(argc > 1 ? argv[1] : "/home") < 0) err("cd: no such directory");
    } else if (str_eq(c, "ls")) {
        if (sh_list_dir(argc > 1 ? argv[1] : ".", io, sizeof(io)) < 0) err("ls: no such directory");
        else out(io, FG);
    } else if (str_eq(c, "cat")) {
        long n;
        if (argc < 2) err("usage: cat <file>");
        else if ((n = sh_read(argv[1], io, sizeof(io))) < 0) err("cat: cannot read file");
        else {
            out(io, FG);
            if (n > 0 && io[n - 1] != '\n') out("\n", FG);
        }
    } else if (str_eq(c, "echo")) {
        join_args(buf, sizeof(buf), argc, argv, 1);
        out(buf, FG);
        out("\n", FG);
    } else if (str_eq(c, "mkdir")) {
        if (argc < 2 || sh_mkdir(argv[1]) != 0) err("mkdir: failed");
    } else if (str_eq(c, "touch")) {
        if (argc < 2 || sh_create(argv[1]) != 0) err("touch: failed");
    } else if (str_eq(c, "write")) {
        if (argc < 3) {
            err("usage: write <file> <text>");
        } else {
            join_args(buf, sizeof(buf), argc, argv, 2);
            str_cat(buf, "\n", sizeof(buf));
            if (sh_write_file(argv[1], buf, str_len(buf)) < 0) err("write: failed");
        }
    } else if (str_eq(c, "stat")) {
        if (argc < 2 || sh_stat(argv[1], &st) != 0) {
            err("stat: no such path");
        } else {
            out(st.type == SH_NODE_DIR ? "directory" : "file", FG);
            out("  size=", FG_DIM);
            out(fmt_u64(buf, st.size, 10), FG);
            out(st.readonly ? "  read-only\n" : "  writable\n", FG_DIM);
        }
    } else if (str_eq(c, "ps")) {
        if (sh_list_procs(io, sizeof(io)) >= 0) out(io, FG);
    } else if (str_eq(c, "run")) {
        if (argc < 2) err("usage: run <program> [args]");
        else {
            join_args(buf, sizeof(buf), argc, argv, 2);
            run_program(argv[1], buf);
        }
    } else if (str_eq(c, "random")) {
        uint8_t r[32];
        if (sh_read_at("/dev/random", 0, (char *)r, sizeof(r)) != (long)sizeof(r)) {
            err("random: /dev/random unavailable");
        } else {
            static const char hx[] = "0123456789abcdef";
            for (int i = 0; i < 32; i++) {
                putc_col(hx[r[i] >> 4], FG);
                putc_col(hx[r[i] & 15], FG);
            }
            out("\n", FG);
            mem_wipe(r, sizeof(r));
        }
    } else if (str_eq(c, "about")) {
        out("Shusha Terminal - kernel: ICDA (github.com/elabbas0/ICDA)\n", FG_DIM);
    } else if (str_eq(c, "exit")) {
        ui_close();
        sh_exit(0);
    } else {
        char path[96];
        if (c[0] == '/') str_copy(path, c, sizeof(path));
        else {
            str_copy(path, "/apps/", sizeof(path));
            str_cat(path, c, sizeof(path));
            str_cat(path, ".app", sizeof(path));
        }
        if (sh_stat(path, &st) != 0) {
            out(c, FG_ERR);
            err(": command not found");
        } else {
            join_args(buf, sizeof(buf), argc, argv, 1);
            run_program(path, buf);
        }
    }
}

/* ---- rendering --------------------------------------------------------- */

static void render(void) {
    surface_t *s = ui_surface();
    int y0 = -1, y1 = -1;

    if (full_redraw) {
        draw_fill(s, 0, 0, s->w, s->h, BG);
        mark_all();
        full_redraw = 0;
        y0 = 0;
        y1 = s->h;
    }
    for (int r = 0; r < ROWS; r++) {
        if (!dirty[r]) continue;
        int y = PAD + r * GLYPH_H;
        draw_fill(s, 0, y, s->w, GLYPH_H, BG);
        for (int c = 0; c < COLS; c++) {
            if (cells[r][c] != ' ') draw_char(s, PAD + c * GLYPH_W, y, cells[r][c], colors[r][c]);
        }
        if (r == cur_r) draw_fill(s, PAD + cur_c * GLYPH_W, y + GLYPH_H - 3, GLYPH_W, 2, COL_ACCENT_HI);
        if (y0 < 0) y0 = y;
        if (y + GLYPH_H > y1) y1 = y + GLYPH_H;
        dirty[r] = 0;
    }
    if (ctx_open) {
        int hover = menu_hit(&ctx_menu, ctx_x, ctx_y, mouse_x, mouse_y);
        menu_draw(s, ctx_x, ctx_y, &ctx_menu, hover);
        y0 = 0;
        y1 = s->h;
    }
    if (y0 >= 0) ui_present_rect(0, y0, s->w, y1 - y0);
}

static void close_menu(void) {
    ctx_open = 0;
    full_redraw = 1;
}

static void menu_action(int i) {
    close_menu();
    if (i == 0) {
        clear_screen();
        prompt();
        out(line, FG);
    } else if (i == 1) {
        (void)sh_spawn("/apps/terminal.app");
    } else if (i == 2) {
        ui_close();
        sh_exit(0);
    }
}

static void key(uint32_t k) {
    /* Swallow arrow-key escape sequences (ESC [ x); no line editing yet. */
    if (esc_state == 1) {
        esc_state = k == '[' ? 2 : 0;
        return;
    }
    if (esc_state == 2) {
        esc_state = (k >= '0' && k <= '9') ? 3 : 0;
        return;
    }
    if (esc_state == 3) {
        if (k == '~' || k < '0' || k > '9') esc_state = 0;
        return;
    }
    if (k == 27) {
        if (ctx_open) close_menu();
        else esc_state = 1;
        return;
    }
    if (k == '\n' || k == '\r') {
        putc_col('\n', FG);
        line[line_len] = '\0';
        execute(line);
        line_len = 0;
        line[0] = '\0';
        prompt();
    } else if (k == '\b' || k == 127) {
        if (line_len > 0) {
            line[--line_len] = '\0';
            putc_col('\b', FG);
        }
    } else if (k >= 32 && k <= 126 && line_len < (int)sizeof(line) - 1) {
        line[line_len++] = (char)k;
        line[line_len] = '\0';
        putc_col((char)k, FG);
    }
}

int main(void) {
    ui_event_t ev;

    if (ui_open("Terminal", WIN_W, WIN_H) != 0) return 1;
    clear_screen();
    out("Shusha Terminal\n", FG_OK);
    out("type 'help' for commands\n\n", FG_DIM);
    prompt();
    render();

    for (;;) {
        ui_wait(&ev);
        do {
            if (ev.type == UI_EV_CLOSE) {
                ui_close();
                return 0;
            }
            if (ev.type == UI_EV_KEY) {
                if (!ctx_open) key(ev.key);
            } else if (ev.type == UI_EV_MOUSE) {
                uint32_t pressed = ev.buttons & ~prev_buttons;
                int moved = ev.x != mouse_x || ev.y != mouse_y;
                mouse_x = ev.x;
                mouse_y = ev.y;
                prev_buttons = ev.buttons;
                if (pressed & UI_BTN_RIGHT) {
                    surface_t *s = ui_surface();
                    ctx_x = ev.x;
                    ctx_y = ev.y;
                    if (ctx_x + menu_width(&ctx_menu) > s->w) ctx_x = s->w - menu_width(&ctx_menu);
                    if (ctx_y + menu_height(&ctx_menu) > s->h) ctx_y = s->h - menu_height(&ctx_menu);
                    ctx_open = 1;
                } else if ((pressed & UI_BTN_LEFT) && ctx_open) {
                    int hit = menu_hit(&ctx_menu, ctx_x, ctx_y, ev.x, ev.y);
                    if (hit >= 0) menu_action(hit);
                    else close_menu();
                } else if (!(moved && ctx_open)) {
                    continue;
                }
            }
        } while (ui_poll(&ev));
        render();
    }
}
