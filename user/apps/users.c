/* users: account management from the console (authd does the work).
 *
 *   users list | whoami
 *   users setup <name>            first account (administrator)
 *   users add <name> [admin]      administrators; asks for your password
 *   users del <name>              administrators
 *   users reset <name>            administrators: set someone's password
 *   users passwd                  change your own password
 *   users check <name>            check a password (no session)
 *   users recover <name>          recovery key -> new password
 *
 * Passwords are always asked for, never taken from the command line. */
#include "lib.h"
#include "svc.h"
#include "authproto.h"

static char reply[1024];

static int auth(const char *verb, const char *data) {
    int status = 1;
    long n = svc_request(AUTH_QUEUE, AUTH_IO_DIR, AUTH_BOX, verb, data, data ? str_len(data) : 0, reply,
                         sizeof(reply), &status);
    if (n < 0) {
        print("users: ");
        print(reply);
        return 1;
    }
    return status;
}

static void ask(const char *prompt, char *buf, uint64_t cap) {
    print(prompt);
    read_secret(buf, cap);
}

static void add(char *data, uint64_t cap, const char *s) {
    str_cat(data, s, cap);
    str_cat(data, "\n", cap);
}

/* Show the answer as sentences rather than tab-separated fields. */
static void show(void) {
    if (str_starts(reply, "fail\tlocked\t")) {
        print("refused: too many attempts, try again in ");
        print(reply + 12);
    } else if (str_starts(reply, "fail\twrong")) {
        print("refused: wrong password\n");
    } else if (str_starts(reply, "fail\t")) {
        print("refused: ");
        print(reply + 5);
    } else {
        print(reply);
    }
}

int main(int argc, char **argv) {
    char data[400], a[96], b[96], c[96];
    const char *cmd = argc > 1 ? argv[1] : "list";
    int st;
    data[0] = '\0';
    if (str_eq(cmd, "list") || str_eq(cmd, "whoami")) {
        st = auth(cmd, 0);
        show();
        return st;
    }
    if (argc < 3 && !str_eq(cmd, "passwd")) {
        print("usage: users list | whoami | setup <name> | add <name> [admin] | del <name>\n"
              "             reset <name> | passwd | check <name> | recover <name>\n");
        return 1;
    }
    if (str_eq(cmd, "setup") || str_eq(cmd, "check")) {
        ask("password: ", a, sizeof(a));
        add(data, sizeof(data), argv[2]);
        add(data, sizeof(data), a);
        st = auth(str_eq(cmd, "setup") ? "setup" : "login", data);
    } else if (str_eq(cmd, "add")) {
        ask("your password: ", a, sizeof(a));
        ask("new account's password: ", b, sizeof(b));
        add(data, sizeof(data), a);
        add(data, sizeof(data), argv[2]);
        add(data, sizeof(data), b);
        add(data, sizeof(data), argc > 3 && str_eq(argv[3], "admin") ? "1" : "0");
        st = auth("create", data);
    } else if (str_eq(cmd, "del")) {
        ask("your password: ", a, sizeof(a));
        add(data, sizeof(data), a);
        add(data, sizeof(data), argv[2]);
        st = auth("delete", data);
    } else if (str_eq(cmd, "reset")) {
        ask("your password: ", a, sizeof(a));
        ask("their new password: ", b, sizeof(b));
        add(data, sizeof(data), a);
        add(data, sizeof(data), argv[2]);
        add(data, sizeof(data), b);
        st = auth("reset", data);
    } else if (str_eq(cmd, "passwd")) {
        ask("current password: ", a, sizeof(a));
        ask("new password: ", b, sizeof(b));
        add(data, sizeof(data), a);
        add(data, sizeof(data), b);
        st = auth("passwd", data);
    } else if (str_eq(cmd, "recover")) {
        ask("recovery key: ", a, sizeof(a));
        ask("new password: ", c, sizeof(c));
        add(data, sizeof(data), argv[2]);
        add(data, sizeof(data), a);
        add(data, sizeof(data), c);
        st = auth("recover", data);
    } else {
        print("users: unknown command\n");
        return 1;
    }
    mem_wipe(data, sizeof(data));
    mem_wipe(a, sizeof(a));
    mem_wipe(b, sizeof(b));
    mem_wipe(c, sizeof(c));
    show();
    return st;
}
