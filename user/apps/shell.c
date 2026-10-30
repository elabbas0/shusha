/* Shusha text shell. */
#include "lib.h"

#define LINE_MAX 256
#define ARGS_MAX 16
#define BUF_MAX  8192

static char io[BUF_MAX];

static void err(const char *cmd, const char *what) {
    print(cmd);
    print(": ");
    print(what);
    print("\n");
}

static int need(int argc, int n, const char *usage) {
    if (argc >= n) return 1;
    print("usage: ");
    print(usage);
    print("\n");
    return 0;
}

static void cmd_help(void) {
    print("commands:\n"
          "  help              this list\n"
          "  clear             clear the screen\n"
          "  pwd | cd <dir>    show / change directory\n"
          "  ls [dir]          list a directory\n"
          "  cat <file>        print a file\n"
          "  echo <text>       print text\n"
          "  mkdir <dir>       create a directory\n"
          "  touch <file>      create an empty file\n"
          "  write <file> <t>  replace a file's contents\n"
          "  stat <path>       file metadata (owner, permissions)\n"
          "  id                show your user id\n"
          "  su <uid>          switch to a user id (root only; one way)\n"
          "  chmod <oct> <p>   change permissions (owner or root)\n"
          "  chown <uid> <p>   change owner (root only)\n"
          "  ps                list processes\n"
          "  run <app> [args]  start a program and wait for it\n"
          "  random [n]        n bytes from the kernel CSPRNG (/dev/random)\n"
          "  sync              flush the writable filesystem\n"
          "  about             system information\n"
          "  poweroff | reboot\n");
}

static void cmd_ls(int argc, char **argv) {
    long n = sh_list_dir(argc > 1 ? argv[1] : ".", io, sizeof(io));
    if (n < 0) {
        err("ls", "no such directory");
        return;
    }
    print(io);
}

static void cmd_cat(int argc, char **argv) {
    uint64_t off = 0;
    if (!need(argc, 2, "cat <file>")) return;
    for (;;) {
        long n = sh_read_at(argv[1], off, io, sizeof(io) - 1);
        if (n < 0) {
            if (off == 0) err("cat", "cannot read file");
            break;
        }
        if (n == 0) break;
        io[n] = '\0';
        print(io);
        off += (uint64_t)n;
        if ((uint64_t)n < sizeof(io) - 1) break;
    }
}

static void cmd_echo(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (i > 1) print(" ");
        print(argv[i]);
    }
    print("\n");
}

static void cmd_write(int argc, char **argv) {
    io[0] = '\0';
    if (!need(argc, 3, "write <file> <text>")) return;
    for (int i = 2; i < argc; i++) {
        if (i > 2) str_cat(io, " ", sizeof(io));
        str_cat(io, argv[i], sizeof(io));
    }
    str_cat(io, "\n", sizeof(io));
    if (sh_write_file(argv[1], io, str_len(io)) < 0) err("write", "failed");
}

static void cmd_stat(int argc, char **argv) {
    sh_stat_t st;
    if (!need(argc, 2, "stat <path>")) return;
    if (sh_stat(argv[1], &st) != 0) {
        err("stat", "no such path");
        return;
    }
    {
        char m[12];
        print(st.type == SH_NODE_DIR ? "d" : "-");
        print(fmt_mode(m, st.mode));
        print("  owner=");
        print_u64(st.uid);
        print("  size=");
        print_u64(st.size);
        print("  inode=");
        print_u64(st.inode);
        print(st.readonly ? "  read-only image\n" : "\n");
    }
}

static void cmd_run(int argc, char **argv) {
    char args[LINE_MAX];
    long pid;
    if (!need(argc, 2, "run <app> [args]")) return;
    args[0] = '\0';
    for (int i = 2; i < argc; i++) {
        if (i > 2) str_cat(args, " ", sizeof(args));
        str_cat(args, argv[i], sizeof(args));
    }
    pid = args[0] ? sh_spawn_args(argv[1], args) : sh_spawn(argv[1]);
    if (pid < 0) {
        err("run", "cannot start program");
        return;
    }
    (void)sh_waitpid((uint64_t)pid);
}

static void cmd_random(int argc, char **argv) {
    uint64_t want = 32;
    uint8_t buf[256];
    long n;
    if (argc > 1) {
        want = 0;
        for (const char *p = argv[1]; *p >= '0' && *p <= '9'; p++) want = want * 10 + (uint64_t)(*p - '0');
        if (want == 0 || want > sizeof(buf)) want = 32;
    }
    n = sh_read_at("/dev/random", 0, (char *)buf, want);
    if (n <= 0) {
        err("random", "/dev/random unavailable");
        return;
    }
    print_hex(buf, (uint64_t)n);
    print("\n");
    mem_wipe(buf, sizeof(buf));
}

static uint64_t parse_u64(const char *p) {
    uint64_t v = 0;
    while (*p >= '0' && *p <= '9') v = v * 10 + (uint64_t)(*p++ - '0');
    return v;
}

static uint64_t parse_oct(const char *p) {
    uint64_t v = 0;
    while (*p >= '0' && *p <= '7') v = v * 8 + (uint64_t)(*p++ - '0');
    return v;
}

static void prompt(void) {
    char cwd[128];
    if (sh_getcwd(cwd, sizeof(cwd)) < 0) str_copy(cwd, "?", sizeof(cwd));
    print(sh_getuid() == 0 ? "root@shusha:" : "shusha:");
    print(cwd);
    print(sh_getuid() == 0 ? "# " : "$ ");
}

static void run_line(char *line) {
    char *argv[ARGS_MAX];
    int argc = split_args(line, argv, ARGS_MAX);
    const char *c;

    if (argc == 0) return;
    c = argv[0];
    if (str_eq(c, "help")) cmd_help();
    else if (str_eq(c, "clear")) sh_clear();
    else if (str_eq(c, "pwd")) {
        char cwd[128];
        if (sh_getcwd(cwd, sizeof(cwd)) >= 0) {
            print(cwd);
            print("\n");
        }
    } else if (str_eq(c, "cd")) {
        if (sh_chdir(argc > 1 ? argv[1] : "/home") < 0) err("cd", "no such directory");
    } else if (str_eq(c, "ls")) cmd_ls(argc, argv);
    else if (str_eq(c, "cat")) cmd_cat(argc, argv);
    else if (str_eq(c, "echo")) cmd_echo(argc, argv);
    else if (str_eq(c, "mkdir")) {
        if (need(argc, 2, "mkdir <dir>") && sh_mkdir(argv[1]) != 0) err("mkdir", "failed");
    } else if (str_eq(c, "touch")) {
        if (need(argc, 2, "touch <file>") && sh_create(argv[1]) != 0) err("touch", "failed");
    } else if (str_eq(c, "write")) cmd_write(argc, argv);
    else if (str_eq(c, "stat")) cmd_stat(argc, argv);
    else if (str_eq(c, "ps")) {
        if (sh_list_procs(io, sizeof(io)) >= 0) print(io);
    } else if (str_eq(c, "run")) cmd_run(argc, argv);
    else if (str_eq(c, "random")) cmd_random(argc, argv);
    else if (str_eq(c, "id")) {
        print("uid=");
        print_u64((uint64_t)sh_getuid());
        print("\n");
    } else if (str_eq(c, "su")) {
        if (need(argc, 2, "su <uid>") && sh_setuid(parse_u64(argv[1])) != 0) err("su", "permission denied");
    } else if (str_eq(c, "chmod")) {
        if (need(argc, 3, "chmod <octal> <path>") && sh_chmod(argv[2], parse_oct(argv[1])) != 0) err("chmod", "permission denied");
    } else if (str_eq(c, "chown")) {
        if (need(argc, 3, "chown <uid> <path>") && sh_chown(argv[2], parse_u64(argv[1])) != 0) err("chown", "permission denied");
    }
    else if (str_eq(c, "sync")) {
        if (sh_sync() != 0) err("sync", "failed");
    } else if (str_eq(c, "about")) {
        print("Shusha - security-first multi-user OS\n"
              "kernel: ICDA (https://github.com/elabbas0/ICDA)\n");
    } else if (str_eq(c, "poweroff")) (void)sh_power(0);
    else if (str_eq(c, "reboot")) (void)sh_power(1);
    else {
        /* Any other word runs /apps/<word>.app with the remaining args. */
        char path[96];
        sh_stat_t st;
        str_copy(path, "/apps/", sizeof(path));
        str_cat(path, c, sizeof(path));
        str_cat(path, ".app", sizeof(path));
        if (c[0] != '/' && sh_stat(path, &st) == 0) {
            char *run_argv[ARGS_MAX + 1];
            run_argv[0] = "run";
            run_argv[1] = path;
            for (int i = 1; i < argc; i++) run_argv[i + 1] = argv[i];
            cmd_run(argc + 1, run_argv);
        } else {
            err(c, "unknown command (try 'help')");
        }
    }
}

int main(void) {
    char line[LINE_MAX];

    (void)sh_chdir("/home");
    if (sh_read("/etc/motd.txt", io, sizeof(io)) > 0) print(io);
    print("shell ready\n");
    for (;;) {
        prompt();
        if (sh_readline(line, sizeof(line)) < 0) {
            sh_sleep(10);
            continue;
        }
        run_line(line);
    }
}
