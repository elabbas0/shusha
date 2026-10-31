/* Shusha PID 1.
 *
 * The kernel spawns /sbin/init.app once per VT era with argv[1] set to
 * "gui" or "text".  Init starts the services (authd, workspaced), then
 * the foreground program - the window manager, whose first window is the
 * login screen, or the text login - waits for it and exits with its
 * code; the kernel respawns init (and drops to a recovery console after
 * repeated failures).
 *
 * Test images (make lan.iso) build this with SHUSHA_TEST_IMAGE: the text
 * console then starts a root shell directly, so scripts can drive many
 * machines.  Release images never contain that path.
 */
#include "lib.h"

int main(int argc, char **argv) {
#ifdef SHUSHA_TEST_IMAGE
    const char *target = "/apps/shell.app";
#else
    const char *target = "/sbin/login.app";
#endif
    sh_stat_t st;
    long pid;

    if (argc > 1 && str_eq(argv[1], "gui") && sh_stat("/apps/wm.app", &st) == 0) {
        target = "/apps/wm.app";
    }

    /* Services.  A second copy (init is respawned per VT era) finds its
     * queue or port taken and exits. */
    if (sh_stat("/sbin/authd.app", &st) == 0) (void)sh_spawn("/sbin/authd.app");
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
