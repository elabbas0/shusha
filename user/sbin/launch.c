/* launch [-w] <uid> <dir> <program> [args]: start a program as another user.
 *
 * Used by the window manager and the text login (both root) to run the
 * login screen and the signed-in user's programs under their own uid.
 * With -w it waits for the program and returns its exit code (the text
 * login waits for the user's shell this way).  The kernel only lets root
 * change uid, so for anyone else this does nothing. */
#include "lib.h"

int main(int argc, char **argv) {
    char args[160];
    uint64_t uid = 0;
    int wait = argc > 1 && str_eq(argv[1], "-w"), a = wait ? 2 : 1;
    long pid;
    if (argc < a + 3 || sh_getuid() != 0) return 1;
    for (const char *p = argv[a]; *p >= '0' && *p <= '9'; p++) uid = uid * 10 + (uint64_t)(*p - '0');
    if (sh_setuid(uid) != 0 || sh_getuid() != (long)uid) return 1;
    (void)sh_chdir(argv[a + 1]);
    args[0] = '\0';
    for (int i = a + 3; i < argc; i++) {
        if (i > a + 3) str_cat(args, " ", sizeof(args));
        str_cat(args, argv[i], sizeof(args));
    }
    pid = args[0] ? sh_spawn_args(argv[a + 2], args) : sh_spawn(argv[a + 2]);
    if (pid < 0) return 1;
    return wait ? (int)sh_waitpid((uint64_t)pid) : 0;
}
