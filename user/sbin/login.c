/* login: text-console sign-in.
 *
 * Asks for a name and password (not echoed), lets authd check them, runs
 * the user's shell under their uid in their home, and when the shell
 * exits ends whatever the user left running.  On a machine with no
 * accounts yet it first creates the administrator. */
#include "lib.h"
#include "svc.h"
#include "authproto.h"

static char reply[512];

static long auth(const char *verb, const char *data, int *status) {
    return svc_request(AUTH_QUEUE, AUTH_IO_DIR, AUTH_BOX, verb, data, data ? str_len(data) : 0, reply,
                       sizeof(reply), status);
}

static void read_line(const char *prompt, char *buf, uint64_t cap) {
    print(prompt);
    while (sh_readline(buf, cap) < 0) sh_sleep(5);
}

/* "fail\t<why>[\t<n>]" -> printed reason */
static void say_fail(void) {
    char *p = reply;
    while (*p && *p != '\t') p++;
    if (*p) p++;
    if (str_starts(p, "locked")) {
        print("Too many attempts; try again in ");
        p += 7;
        while (*p >= '0' && *p <= '9') {
            char c[2] = { *p++, 0 };
            print(c);
        }
        print(" s.\n");
    } else if (str_starts(p, "wrong")) {
        print("Login incorrect.\n");
    } else {
        print(p);
    }
}

static void first_account(void) {
    char name[32], pw[96], pw2[96], data[200];
    int status;
    print("\nNo accounts yet. Create the administrator account for this machine.\n");
    for (;;) {
        read_line("name: ", name, sizeof(name));
        print("password (at least 8 characters): ");
        read_secret(pw, sizeof(pw));
        print("repeat password: ");
        read_secret(pw2, sizeof(pw2));
        if (!str_eq(pw, pw2)) {
            print("The passwords do not match.\n");
            continue;
        }
        str_copy(data, name, sizeof(data));
        str_cat(data, "\n", sizeof(data));
        str_cat(data, pw, sizeof(data));
        str_cat(data, "\n", sizeof(data));
        auth("setup", data, &status);
        mem_wipe(data, sizeof(data));
        mem_wipe(pw, sizeof(pw));
        mem_wipe(pw2, sizeof(pw2));
        if (!status && str_starts(reply, "ok\t")) {
            print("Account created. Your recovery key (shown once, write it down):\n  ");
            print(reply + 3);
            mem_wipe(reply, sizeof(reply));
            return;
        }
        say_fail();
    }
}

int main(void) {
    for (;;) {
        char name[32], pw[96], data[160], args[96], n[16], *f[5];
        int status, nf = 0;
        if (auth("list", 0, 0) < 0) {
            print("login: the account service is not running\n");
            sh_sleep(100);
            continue;
        }
        if (!reply[0]) first_account();
        read_line("\nshusha login: ", name, sizeof(name));
        if (!name[0]) continue;
        print("password: ");
        read_secret(pw, sizeof(pw));
        str_copy(data, name, sizeof(data));
        str_cat(data, "\n", sizeof(data));
        str_cat(data, pw, sizeof(data));
        str_cat(data, "\n", sizeof(data));
        auth("login", data, &status);
        mem_wipe(data, sizeof(data));
        mem_wipe(pw, sizeof(pw));
        if (status || !str_starts(reply, "ok\t")) {
            say_fail();
            continue;
        }
        /* ok \t name \t uid \t admin|user \t home */
        for (char *p = reply; *p && nf < 5;) {
            f[nf++] = p;
            while (*p && *p != '\t' && *p != '\n') p++;
            if (*p) *p++ = '\0';
        }
        if (nf < 5) continue;
        str_copy(args, "-w ", sizeof(args));
        str_cat(args, f[2], sizeof(args));
        str_cat(args, " ", sizeof(args));
        str_cat(args, f[4], sizeof(args));
        str_cat(args, " /apps/shell.app", sizeof(args));
        str_copy(n, f[2], sizeof(n));
        print("Welcome, ");
        print(f[1]);
        print(".\n");
        {
            long pid = sh_spawn_args("/sbin/launch.app", args);
            if (pid > 0) (void)sh_waitpid((uint64_t)pid);
        }
        /* Signed out: nothing of theirs keeps running. */
        (void)sh_write_file("/dev/killuid", n, str_len(n));
        print("Signed out.\n");
    }
}
