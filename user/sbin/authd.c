/* authd: accounts, passwords and sign-in for Shusha.
 *
 * Runs as root, started by init before anything that needs it.  Holds the
 * account database (/var/auth/users) and a random machine key (pepper),
 * both readable by root only.  Programs talk to it through svc.h; the
 * kernel stamps every request with the caller's pid and uid, which is
 * what authd trusts.
 *
 * Security choices (ICDA's accounts used PBKDF2 in the kernel):
 *  - Passwords and recovery keys are stored only as Argon2id hashes
 *    (16 MiB, 2 passes) with a per-account salt, the machine pepper as
 *    Argon2's secret input, and the account name bound in as associated
 *    data - a stolen database is useless without the pepper, and a hash
 *    cannot be moved to another account.
 *  - Failed attempts lock the account out for 5 s, doubling up to 15 min,
 *    before any hash is computed; comparisons are constant-time; secrets
 *    are wiped after use.
 *  - Administrators are not root: being an admin only lets you manage
 *    accounts here, and every such change asks for your password again.
 *  - No automatic sign-in.  The login screen runs as an unprivileged uid
 *    and never sees a hash; authd itself tells the window manager whom to
 *    start a session for.
 */
#include "lib.h"
#include "svc.h"
#include "authproto.h"
#include "argon2.h"
#include "ui_proto.h"

#define AUTH_DIR   "/var/auth"
#define USERS_FILE AUTH_DIR "/users"
#define PEPPER     AUTH_DIR "/pepper"
#define MAXACCT    32
#define MEM_KIB    16384
#define PASSES     2
#define MIN_PW     8

typedef struct {
    int used;
    char name[24];
    uint32_t uid, flags;
    uint8_t salt[16], hash[32];
    uint8_t rsalt[16], rhash[32];   /* recovery key */
    uint32_t fails;
    uint64_t locked_until;          /* runtime only */
} acct_t;

#define DB_MAGIC 0x31445541u /* "AUD1" */

static struct {
    uint32_t magic;
    acct_t a[MAXACCT];
} db;

static uint8_t pepper[32];
static uint8_t work[MEM_KIB * 1024] __attribute__((aligned(16)));
static uint8_t inbuf[4096];
static char answer[2048];
static uint64_t answer_len;

static void say(const char *s) {
    while (*s && answer_len + 1 < sizeof(answer)) answer[answer_len++] = *s++;
    answer[answer_len] = '\0';
}

static void say_u(uint64_t v) {
    char b[24];
    say(fmt_u64(b, v, 10));
}

static void log_line(const char *a, const char *b) {
    print("authd: ");
    print(a);
    if (b) print(b);
    print("\n");
}

static void rand_bytes(void *p, uint64_t n) {
    if (sh_read_at("/dev/random", 0, (char *)p, n) != (long)n) {
        log_line("no /dev/random", 0);
        sh_exit(1);
    }
}

static void save_db(void) {
    (void)sh_write_file(USERS_FILE, &db, sizeof(db));
    (void)sh_chmod(USERS_FILE, 0600);
}

static int count_accounts(void) {
    int n = 0;
    for (int i = 0; i < MAXACCT; i++) n += db.a[i].used;
    return n;
}

static acct_t *by_name(const char *name) {
    for (int i = 0; i < MAXACCT; i++) {
        if (db.a[i].used && str_eq(db.a[i].name, name)) return &db.a[i];
    }
    return 0;
}

static acct_t *by_uid(uint32_t uid) {
    for (int i = 0; i < MAXACCT; i++) {
        if (db.a[i].used && db.a[i].uid == uid) return &db.a[i];
    }
    return 0;
}

/* ---- hashing ----------------------------------------------------------- */

static void kdf(uint8_t out[32], const char *secret, const uint8_t salt[16], const char *purpose, const char *name) {
    char ad[64];
    str_copy(ad, purpose, sizeof(ad));
    str_cat(ad, name, sizeof(ad));
    (void)sc_argon2id(out, 32, (const uint8_t *)secret, (uint32_t)str_len(secret), salt, 16, pepper, 32,
                      (const uint8_t *)ad, (uint32_t)str_len(ad), PASSES, MEM_KIB, 1, work);
}

static void set_password(acct_t *a, const char *pw) {
    rand_bytes(a->salt, 16);
    kdf(a->hash, pw, a->salt, "shusha-login:", a->name);
}

/* A fresh recovery key, returned as XXXXX-XXXXX-XXXXX-XXXXX (100 bits). */
static void new_recovery(acct_t *a, char key[24]) {
    static const char alpha[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
    uint8_t r[20];
    int k = 0;
    rand_bytes(r, sizeof(r));
    for (int i = 0; i < 20; i++) {
        if (i && i % 5 == 0) key[k++] = '-';
        key[k++] = alpha[r[i] & 31];
    }
    key[k] = '\0';
    rand_bytes(a->rsalt, 16);
    kdf(a->rhash, key, a->rsalt, "shusha-recovery:", a->name);
    mem_wipe(r, sizeof(r));
}

/* Rate-limited check.  Returns 1 if right, 0 if wrong, -seconds while
 * the account is locked out (nothing is computed then). */
static long check(acct_t *a, const char *secret, int recovery) {
    uint8_t h[32];
    int ok;
    uint64_t now = sh_ticks();
    if (a->locked_until > now) return -(long)((a->locked_until - now + 99) / 100);
    kdf(h, secret, recovery ? a->rsalt : a->salt, recovery ? "shusha-recovery:" : "shusha-login:", a->name);
    ok = mem_eq_ct(h, recovery ? a->rhash : a->hash, 32);
    mem_wipe(h, 32);
    if (ok) {
        a->fails = 0;
        a->locked_until = 0;
        return 1;
    }
    a->fails++;
    if (a->fails >= 3) {
        uint64_t secs = 5;
        for (uint32_t i = 3; i < a->fails && secs < 900; i++) secs *= 2;
        if (secs > 900) secs = 900;
        a->locked_until = now + secs * 100;
    }
    save_db();
    return 0;
}

static void say_check_fail(long r) {
    if (r < 0) {
        say("fail\tlocked\t");
        say_u((uint64_t)-r);
        say("\n");
    } else {
        say("fail\twrong\n");
    }
}

/* ---- input ------------------------------------------------------------- */

/* Split the data part into lines (in place). */
static int fields(svc_req_t *req, char **f, int max) {
    int n = 0;
    char *p = (char *)req->data;
    if (!p) return 0;
    while (n < max && p < (char *)req->data + req->dlen) {
        f[n++] = p;
        while (p < (char *)req->data + req->dlen && *p != '\n') p++;
        if (p < (char *)req->data + req->dlen) *p++ = '\0';
    }
    req->data[req->dlen] = 0;
    return n;
}

static int valid_name(const char *n) {
    int len = 0;
    if (str_eq(n, "root") || str_eq(n, "greeter") || str_eq(n, "admin-recovery")) return 0;
    for (; *n; n++, len++) {
        if (!((*n >= 'a' && *n <= 'z') || (*n >= '0' && *n <= '9') || *n == '_' || *n == '-')) return 0;
    }
    return len >= 1 && len <= 23;
}

static int valid_password(const char *p) {
    return str_len(p) >= MIN_PW && str_len(p) <= 128;
}

/* ---- accounts ---------------------------------------------------------- */

static acct_t *create_account(const char *name, const char *pw, uint32_t flags, char key[24]) {
    acct_t *a = 0;
    uint32_t uid = AUTH_FIRST_UID;
    char home[48];
    for (int i = 0; i < MAXACCT; i++) {
        if (db.a[i].used && db.a[i].uid >= uid) uid = db.a[i].uid + 1;
        if (!db.a[i].used && !a) a = &db.a[i];
    }
    if (!a) return 0;
    mem_set(a, 0, sizeof(*a));
    a->used = 1;
    str_copy(a->name, name, sizeof(a->name));
    a->uid = uid;
    a->flags = flags;
    set_password(a, pw);
    new_recovery(a, key);
    save_db();
    /* A private home: 0700, owned by the new user. */
    str_copy(home, "/home/", sizeof(home));
    str_cat(home, name, sizeof(home));
    (void)sh_mkdir(home);
    (void)sh_chown(home, uid);
    (void)sh_chmod(home, 0700);
    log_line("created account ", name);
    return a;
}

static void say_account(const acct_t *a) {
    say(a->name);
    say("\t");
    say_u(a->uid);
    say(a->flags & ACCT_ADMIN ? "\tadmin\t/home/" : "\tuser\t/home/");
    say(a->name);
    say("\n");
}

/* The window manager starts (or unlocks) a session only when authd says
 * so, and it checks that the message came from authd. */
static void tell_wm(const acct_t *a) {
    ui_msg_t m;
    uint64_t q = sh_msg_open(UI_WM_QUEUE);
    if (!q) return;
    mem_set(&m, 0, sizeof(m));
    m.type = UI_SESSION;
    m.session.uid = a->uid;
    m.session.flags = a->flags;
    str_copy(m.session.name, a->name, sizeof(m.session.name));
    (void)sh_msg_send(q, &m);
}

/* An administrator re-entering their own password.  Root (the console of
 * a test image, or system tools) needs no account. */
static acct_t *admin_reauth(svc_req_t *req, const char *pw) {
    static acct_t root_acct = { .used = 1, .name = "root", .flags = ACCT_ADMIN };
    acct_t *me = by_uid(req->uid);
    long r;
    if (req->uid == 0) return &root_acct;
    if (!me || !(me->flags & ACCT_ADMIN)) {
        say("fail\tnot an administrator\n");
        return 0;
    }
    r = check(me, pw, 0);
    if (r != 1) {
        say_check_fail(r);
        return 0;
    }
    return me;
}

static int handle(svc_req_t *req) {
    char *f[6];
    int n = fields(req, f, 6);
    const char *verb = req->line;
    int system_caller = req->uid == 0 || req->uid == GREETER_UID;
    answer_len = 0;
    answer[0] = '\0';

    if (str_eq(verb, "list")) {
        for (int i = 0; i < MAXACCT; i++) {
            if (db.a[i].used) say_account(&db.a[i]);
        }
        return 0;
    }
    if (str_eq(verb, "whoami")) {
        acct_t *a = by_uid(req->uid);
        if (a) say_account(a);
        return a ? 0 : 1;
    }
    if (str_eq(verb, "setup")) {
        /* First boot only, and only from the login screen or root. */
        char key[24];
        acct_t *a;
        if (count_accounts() || !system_caller) {
            say("fail\tsetup is only for the first account\n");
            return 1;
        }
        if (n < 2 || !valid_name(f[0]) || !valid_password(f[1])) {
            say("fail\tname: a-z 0-9 _ -, password: at least 8 characters\n");
            return 1;
        }
        a = create_account(f[0], f[1], ACCT_ADMIN, key);
        if (!a) return 1;
        say("ok\t");
        say(key);
        say("\n");
        mem_wipe(key, sizeof(key));
        return 0;
    }
    if (str_eq(verb, "login")) {
        acct_t *a = n >= 2 ? by_name(f[0]) : 0;
        long r;
        if (!a) {
            /* Same work and answer as a wrong password: names are not
             * confirmed this way. */
            uint8_t h[32], salt[16] = { 0 };
            kdf(h, n >= 2 ? f[1] : "", salt, "shusha-login:", "?");
            say("fail\twrong\n");
            return 1;
        }
        r = check(a, f[1], 0);
        if (r != 1) {
            say_check_fail(r);
            log_line("failed sign-in for ", a->name);
            return 1;
        }
        say("ok\t");
        say_account(a);
        log_line("signed in: ", a->name);
        if (req->uid == GREETER_UID) tell_wm(a);
        return 0;
    }
    if (str_eq(verb, "passwd")) {
        /* own: old, new */
        acct_t *a = by_uid(req->uid);
        long r;
        if (!a || n < 2) {
            say("fail\tno account\n");
            return 1;
        }
        if (!valid_password(f[1])) {
            say("fail\tpassword: at least 8 characters\n");
            return 1;
        }
        r = check(a, f[0], 0);
        if (r != 1) {
            say_check_fail(r);
            return 1;
        }
        set_password(a, f[1]);
        save_db();
        say("ok\n");
        return 0;
    }
    if (str_eq(verb, "create")) {
        /* caller password, name, password, admin(0/1) */
        char key[24];
        acct_t *a;
        if (n < 4) return say("fail\tmissing fields\n"), 1;
        if (!admin_reauth(req, f[0])) return 1;
        if (!valid_name(f[1]) || by_name(f[1])) {
            say("fail\tthat name is taken or not allowed (a-z 0-9 _ -)\n");
            return 1;
        }
        if (!valid_password(f[2])) {
            say("fail\tpassword: at least 8 characters\n");
            return 1;
        }
        a = create_account(f[1], f[2], f[3][0] == '1' ? ACCT_ADMIN : 0, key);
        if (!a) {
            say("fail\ttoo many accounts\n");
            return 1;
        }
        say("ok\t");
        say_u(a->uid);
        say("\t");
        say(key);
        say("\n");
        mem_wipe(key, sizeof(key));
        return 0;
    }
    if (str_eq(verb, "delete")) {
        /* caller password, name.  Files stay, locked to root. */
        acct_t *me, *a;
        char home[48], kill[16];
        int admins = 0;
        if (n < 2) return say("fail\tmissing fields\n"), 1;
        if (!(me = admin_reauth(req, f[0]))) return 1;
        a = by_name(f[1]);
        for (int i = 0; i < MAXACCT; i++) admins += db.a[i].used && (db.a[i].flags & ACCT_ADMIN);
        if (!a || a == me || ((a->flags & ACCT_ADMIN) && admins < 2)) {
            say("fail\tcannot delete that account\n");
            return 1;
        }
        (void)sh_write_file("/dev/killuid", fmt_u64(kill, a->uid, 10), str_len(kill));
        str_copy(home, "/home/", sizeof(home));
        str_cat(home, a->name, sizeof(home));
        (void)sh_chown(home, 0);
        (void)sh_chmod(home, 0700);
        log_line("deleted account ", a->name);
        mem_wipe(a, sizeof(*a));
        save_db();
        say("ok\n");
        return 0;
    }
    if (str_eq(verb, "reset")) {
        /* admin resets someone's password: caller password, name, new */
        acct_t *a;
        if (n < 3) return say("fail\tmissing fields\n"), 1;
        if (!admin_reauth(req, f[0])) return 1;
        if (!(a = by_name(f[1])) || !valid_password(f[2])) {
            say("fail\tno such account, or password too short\n");
            return 1;
        }
        set_password(a, f[2]);
        a->fails = 0;
        a->locked_until = 0;
        save_db();
        say("ok\n");
        return 0;
    }
    if (str_eq(verb, "recover")) {
        /* name, recovery key, new password -> new recovery key */
        char key[24];
        acct_t *a = n >= 3 ? by_name(f[0]) : 0;
        long r;
        if (!a || !valid_password(f[2])) {
            say("fail\twrong\n");
            return 1;
        }
        r = check(a, f[1], 1);
        if (r != 1) {
            say_check_fail(r);
            return 1;
        }
        set_password(a, f[2]);
        new_recovery(a, key);
        save_db();
        say("ok\t");
        say(key);
        say("\n");
        mem_wipe(key, sizeof(key));
        log_line("password recovered for ", a->name);
        return 0;
    }
    say("fail\tunknown request\n");
    return 1;
}

int main(void) {
    uint64_t ctl;
    svc_req_t req;

    (void)sh_mkdir("/var");
    (void)sh_chmod("/var", 0755);
    (void)sh_mkdir(AUTH_DIR);
    (void)sh_mkdir(AUTH_IO_DIR);
    (void)sh_chmod(AUTH_DIR, 0711);
    (void)sh_chmod(AUTH_IO_DIR, 0777);
    if (sh_read_at(PEPPER, 0, (char *)pepper, 32) != 32) {
        rand_bytes(pepper, 32);
        (void)sh_write_file(PEPPER, pepper, 32);
    }
    (void)sh_chmod(PEPPER, 0600);
    if (sh_read_at(USERS_FILE, 0, (char *)&db, sizeof(db)) != (long)sizeof(db) || db.magic != DB_MAGIC) {
        mem_set(&db, 0, sizeof(db));
        db.magic = DB_MAGIC;
    }
    for (int i = 0; i < MAXACCT; i++) db.a[i].locked_until = 0;
    ctl = sh_msg_open(AUTH_QUEUE);
    if (!ctl || sh_msg_owner(ctl) != (long)sh_pid()) {
        log_line("the account queue is owned by another process", 0);
        return 1;
    }
    log_line("ready", 0);
    for (;;) {
        int r = svc_next(ctl, AUTH_IO_DIR, AUTH_BOX, inbuf, sizeof(inbuf), &req);
        if (r == 1) {
            int status = handle(&req);
            mem_wipe(inbuf, sizeof(inbuf));
            svc_reply(AUTH_IO_DIR, AUTH_BOX, &req, answer, answer_len, status);
            mem_wipe(answer, sizeof(answer));
        } else if (r == 0) {
            sh_sleep(2);
        }
    }
}
