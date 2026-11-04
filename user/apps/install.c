/* install: put Shusha on a disk, and manage the saved state (root).
 *
 *   install                  the state store, the disks, what to do next
 *   install <disk>           erase <disk> and install (asks to type ERASE)
 *   install -y <disk>        the same without asking (scripts)
 *   install unlock <code>    open a disk bound to other hardware with its
 *                            recovery code (then restart)
 *   install sync             save now
 *
 * The work is the kernel's (kernel/fs/store.c, through /dev/store): it
 * partitions the disk, writes GRUB and this kernel, makes a machine key
 * bound to this machine's hardware, and from then on saves accounts,
 * homes and the workspace there, encrypted.  The desktop's installer and
 * unlock screen go through authd, which writes the same commands. */
#include "lib.h"

static char st[2048];

static void status(void) {
    if (sh_read("/dev/store", st, sizeof(st) - 1) <= 0) {
        print("install: this kernel has no state store\n");
        st[0] = 0;
    }
}

static const char *field(const char *key, char *out, uint64_t cap) {
    uint64_t kl = str_len(key);
    out[0] = 0;
    for (const char *p = st; *p;) {
        if (str_starts(p, key) && p[kl] == ' ') {
            uint64_t n = 0;
            p += kl + 1;
            while (*p && *p != '\n' && n + 1 < cap) out[n++] = *p++;
            out[n] = 0;
            return out;
        }
        while (*p && *p != '\n') p++;
        if (*p) p++;
    }
    return out;
}

static int command(const char *cmd) {
    return sh_write_file("/dev/store", cmd, str_len(cmd)) < 0 ? -1 : 0;
}

static void show(void) {
    status();
    print(st);
}

static int do_install(const char *disk, int ask) {
    char cmd[32], line[32], code[40], note[96];
    if (sh_getuid() != 0) {
        print("install: only root (or the desktop's installer) can install\n");
        return 1;
    }
    status();
    if (!str_eq(field("can-install", line, sizeof(line)), "yes")) {
        print("install: this kernel cannot install (built without boot files)\n");
        return 1;
    }
    if (ask) {
        print("This erases EVERYTHING on disk ");
        print(disk);
        print(" and installs Shusha on it.\nType ERASE to continue: ");
        if (sh_readline(line, sizeof(line)) <= 0 || !str_eq(line, "ERASE")) {
            print("install: cancelled\n");
            return 1;
        }
    }
    print("install: partitioning, writing GRUB and the kernel, sealing the state...\n");
    str_copy(cmd, "install ", sizeof(cmd));
    str_cat(cmd, disk, sizeof(cmd));
    if (command(cmd) != 0) {
        status();
        print("install: failed: ");
        print(field("note", note, sizeof(note)));
        print("\n");
        return 1;
    }
    status();
    field("recovery", code, sizeof(code));
    print("install: done. Shusha is installed on disk ");
    print(disk);
    print(".\n\nRecovery code (write it down; it is shown only now):\n\n    ");
    print(code);
    print("\n\nThe disk only opens on this machine. If its hardware changes, or the disk\n"
          "moves to another machine, this code unlocks it. Accounts and files made\n"
          "from now on are saved, encrypted. Remove the install media and restart.\n");
    (void)command("forget");
    mem_wipe(code, sizeof(code));
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        show();
        print("\nusage: install <disk> | install unlock <code> | install sync\n");
        return 0;
    }
    if (str_eq(argv[1], "sync")) return command("sync") == 0 ? 0 : 1;
    if (str_eq(argv[1], "unlock")) {
        char cmd[96], note[96];
        if (argc < 3) {
            print("usage: install unlock <recovery code>\n");
            return 1;
        }
        str_copy(cmd, "unlock ", sizeof(cmd));
        for (int i = 2; i < argc; i++) str_cat(cmd, argv[i], sizeof(cmd));
        if (command(cmd) != 0) {
            status();
            print("install: ");
            print(field("note", note, sizeof(note)));
            print("\n");
            return 1;
        }
        /* Services started while the disk was locked hold empty state
         * (no accounts): save the merged tree and restart now. */
        print("install: unlocked and rebound to this machine; restarting\n");
        (void)sh_sync();
        sh_sleep(100);
        (void)sh_power(1);
        return 0;
    }
    if (str_eq(argv[1], "-y") && argc > 2) return do_install(argv[2], 0);
    return do_install(argv[1], 1);
}
