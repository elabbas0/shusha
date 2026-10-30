/* Shusha PID 1.
 *
 * The kernel spawns /sbin/init.app once per VT era with argv[1] set to
 * "gui" or "text".  Init runs the matching foreground program, waits for
 * it and exits with its code; the kernel respawns init (and drops to a
 * recovery console after repeated failures).  It also starts the
 * services (workspaced now; keyd and authd as they land).
 */
#include "lib.h"

int main(int argc, char **argv) {
    const char *target = "/apps/shell.app";
    sh_stat_t st;
    long pid;

    if (argc > 1 && str_eq(argv[1], "gui") && sh_stat("/apps/wm.app", &st) == 0) {
        target = "/apps/wm.app";
    }

    /* Services.  A second copy (init is respawned per VT era) finds its
     * port and control queue taken and exits. */
    if (sh_stat("/sbin/workspaced.app", &st) == 0) (void)sh_spawn("/sbin/workspaced.app");

    pid = sh_spawn(target);
    if (pid < 0) {
        print("init: cannot start ");
        print(target);
        print("\n");
        return 1;
    }
    return (int)sh_waitpid((uint64_t)pid);
}
