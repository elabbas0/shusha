/* fscheck: file ownership and permissions, from an ordinary process.
 * Starts as root, prepares a private file, drops to uid 1000 and checks
 * that the kernel refuses what a normal user must not do. */
#include "lib.h"

static int fails;
static char buf[256];

static void expect(int ok, const char *what) {
    print(ok ? "  ok    " : "  FAIL  ");
    print(what);
    print("\n");
    if (!ok) fails++;
}

int main(void) {
    sh_stat_t st;
    static const char secret[] = "root's private note\n";

    print("fscheck: as root\n");
    expect(sh_getuid() == 0, "starts as uid 0");
    expect(sh_mkdir("/home/fscheck-root") == 0 || sh_stat("/home/fscheck-root", &st) == 0, "root makes a directory");
    expect(sh_write_file("/home/fscheck-root/secret.txt", secret, sizeof(secret) - 1) > 0, "root writes a file");
    expect(sh_chmod("/home/fscheck-root/secret.txt", 0600) == 0, "root makes it private (0600)");
    expect(sh_chmod("/home/fscheck-root", 0700) == 0, "and its directory (0700)");
    expect(sh_stat("/home/fscheck-root/secret.txt", &st) == 0 && st.uid == 0 && st.mode == 0600,
           "stat reports owner 0, mode 0600");

    print("fscheck: as uid 1000\n");
    expect(sh_setuid(1000) == 0 && sh_getuid() == 1000, "root drops to uid 1000");
    expect(sh_read("/home/fscheck-root/secret.txt", buf, sizeof(buf)) < 0, "cannot read root's private file");
    expect(sh_write_file("/home/fscheck-root/secret.txt", "x", 1) < 0, "cannot overwrite it");
    expect(sh_list_dir("/home/fscheck-root", buf, sizeof(buf)) < 0, "cannot list root's private directory");
    expect(sh_chdir("/home/fscheck-root") < 0, "cannot enter it");
    expect(sh_create("/home/fscheck-root/planted") < 0, "cannot create files inside it");
    expect(sh_chmod("/home/fscheck-root/secret.txt", 0666) < 0, "cannot chmod a file it does not own");
    expect(sh_chown("/home/fscheck-root/secret.txt", 1000) < 0, "cannot chown (root only)");
    expect(sh_setuid(0) < 0 && sh_getuid() == 1000, "cannot become root again");
    expect(sh_write_file("/etc/motd.txt", "x", 1) < 0, "cannot write the system image");

    expect(sh_write_file("/home/u1000.txt", "mine\n", 5) > 0, "can create its own file in /home");
    expect(sh_stat("/home/u1000.txt", &st) == 0 && st.uid == 1000, "new file is owned by uid 1000");
    expect(st.mode == 0600, "and private by default (0600)");
    expect(sh_chmod("/home/u1000.txt", 0600) == 0, "owner can chmod its own file");
    expect(sh_read("/home/u1000.txt", buf, sizeof(buf)) == 5, "owner can still read it");
    expect(sh_read("/etc/motd.txt", buf, sizeof(buf)) > 0, "world-readable files stay readable");

    print(fails ? "fscheck: FAILURES\n" : "fscheck: all passed\n");
    return fails ? 1 : 0;
}
