/* seccheck: what an ordinary user may not do (Shusha's syscall gates and
 * account service).  Starts as root, drops to uid 1000, then tries:
 * controlling root's programs, raw disk and display calls, ending a
 * session, reading the account database, becoming root again. */
#include "lib.h"

static int fails;

static void expect(int ok, const char *what) {
    print(ok ? "  ok    " : "  FAIL  ");
    print(what);
    print("\n");
    if (!ok) fails++;
}

int main(void) {
    char buf[64];
    sh_fb_info_t fb;
    long init_pid = 1;

    if (sh_getuid() != 0) {
        print("seccheck: start it as root\n");
        return 1;
    }
    expect(sh_setuid(1000) == 0 && sh_getuid() == 1000, "drops to uid 1000");
    expect((long)sys2(SYS_KILL, (uint64_t)init_pid, 9) == -1, "cannot kill a root process");
    expect((long)sys1(SYS_SUSPEND, (uint64_t)init_pid) == -1, "cannot suspend a root process");
    expect((long)sys2(SYS_MOUNT, 0, (uint64_t)(uintptr_t)"/mnt") == -1, "cannot mount a volume");
    expect((long)sys2(SYS_FORMAT_DEVICE, 0, 1) == -1, "cannot format a disk");
    expect((long)sys2(SYS_INSTALL_SYSTEM, 0, 0) == -1, "cannot install the system");
    mem_set(&fb, 0, sizeof(fb));
    expect((long)sh_map_framebuffer(&fb) == -1, "cannot take over the display");
    expect(sh_write_file("/dev/killuid", "1000", 4) < 0, "cannot end a session (even its own)");
    expect(sh_read("/var/auth/users", buf, sizeof(buf)) < 0, "cannot read the account database");
    expect(sh_read("/var/auth/pepper", buf, sizeof(buf)) < 0, "cannot read the machine password key");
    expect(sh_setuid(0) < 0 && sh_getuid() == 1000, "cannot become root again");
    print(fails ? "seccheck: FAILURES\n" : "seccheck: all passed\n");
    return fails ? 1 : 0;
}
