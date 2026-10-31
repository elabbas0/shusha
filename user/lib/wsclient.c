/* Client side of the workspaced control protocol (wsproto.h), shared by
 * the `ws` command, the Terminal's built-in `ws` and the Workspace app.
 *
 * Personal files stay personal: this code runs as the user, so `put` can
 * only share what the user can read, and `get` can only write where the
 * user can write.  The service never opens a personal path itself. */
#include "lib.h"
#include "wsclient.h"
#include "wsproto.h"

static uint8_t req[256 + WS_MAX_FILE + 1];

static long fail(char *out, uint64_t cap, const char *why) {
    str_copy(out, why, cap);
    return -1;
}

long ws_request(const char *line, const void *data, uint64_t dlen, char *out, uint64_t cap, int *failed) {
    char n[24], in_path[64], out_path[64], box[48];
    uint64_t ctl, me, end, len = str_len(line) + 1;
    ws_ctl_t m;
    long got, owner;

    if (failed) *failed = 0;
    if (len > 256 || dlen > WS_MAX_FILE) return fail(out, cap, "ws: request too large\n");
    mem_copy(req, line, len);
    if (data) mem_copy(req + len, data, dlen);

    fmt_u64(n, sh_pid(), 10);
    str_copy(in_path, WS_IO_DIR "/", sizeof(in_path));
    str_cat(in_path, n, sizeof(in_path));
    str_copy(out_path, in_path, sizeof(out_path));
    str_cat(in_path, ".in", sizeof(in_path));
    str_cat(out_path, ".out", sizeof(out_path));
    /* Private before it holds anything: the service refuses a request
     * file that is not ours or that others could write. */
    (void)sh_create(in_path);
    (void)sh_chmod(in_path, 0600);
    if (sh_write_file(in_path, req, len + (data ? dlen : 0)) < 0) {
        return fail(out, cap, "ws: the workspace service is not running\n");
    }
    mem_wipe(req, len + dlen);

    str_copy(box, "/proc/", sizeof(box));
    str_cat(box, n, sizeof(box));
    str_cat(box, "/ws", sizeof(box));
    me = sh_msg_open(box);
    ctl = sh_msg_open(WS_CTL_QUEUE);
    owner = ctl ? sh_msg_owner(ctl) : -1;
    if (!me || owner <= 0 || owner == (long)sh_pid()) {
        return fail(out, cap, "ws: the workspace service is not running\n");
    }
    mem_set(&m, 0, sizeof(m));
    m.type = WS_CTL_RUN;
    m.pid = (uint32_t)sh_pid();
    if (sh_msg_send(ctl, &m) != 0) return fail(out, cap, "ws: cannot reach the workspace service\n");

    /* Joins and syncs talk to other machines; allow a minute.  Only a
     * reply from the queue's owner counts. */
    end = sh_ticks() + 6000;
    for (;;) {
        long from = sh_msg_recv_from(me, &m);
        if (from == owner && m.type == WS_CTL_DONE && m.pid == (uint32_t)sh_pid()) break;
        if (sh_ticks() > end) return fail(out, cap, "ws: timed out waiting for the workspace service\n");
        if (from <= 0) sh_sleep(2);
    }
    got = sh_read(out_path, out, cap - 1);
    if (got < 0) got = 0;
    out[got] = '\0';
    (void)sh_write_file(out_path, "", 0);
    if (failed) *failed = m.status != 0;
    return got;
}

static const char *base_name(const char *path) {
    const char *b = path;
    for (const char *c = path; *c; c++) {
        if (*c == '/') b = c + 1;
    }
    return b;
}

long ws_run(const char *cmdline, char *out, uint64_t cap) {
    static char data[WS_MAX_FILE + 1];
    char line[256], *argv[4];
    int argc, failed;
    long n;

    str_copy(line, cmdline, sizeof(line));
    argc = split_args(line, argv, 4);
    if (argc == 2 && str_eq(argv[0], "put")) {
        char cmd[96];
        sh_stat_t st;
        if (sh_stat(argv[1], &st) != 0 || st.type == SH_NODE_DIR) return fail(out, cap, "ws: no such file\n");
        n = sh_read_at(argv[1], 0, data, sizeof(data));
        if (n < 0) return fail(out, cap, "ws: you cannot read that file\n");
        if (n > WS_MAX_FILE) return fail(out, cap, "ws: file too large (8 KB limit for now)\n");
        str_copy(cmd, "put ", sizeof(cmd));
        str_cat(cmd, base_name(argv[1]), sizeof(cmd));
        n = ws_request(cmd, data, (uint64_t)n, out, cap, 0);
        mem_wipe(data, sizeof(data));
        return n;
    }
    if (argc == 3 && str_eq(argv[0], "get")) {
        char cmd[96];
        str_copy(cmd, "raw ", sizeof(cmd));
        str_cat(cmd, argv[1], sizeof(cmd));
        n = ws_request(cmd, 0, 0, data, sizeof(data), &failed);
        if (n < 0 || failed) {
            str_copy(out, data, cap);
            return -1;
        }
        /* Workspace content stays private once saved: 0600 before it
         * holds anything. */
        (void)sh_create(argv[2]);
        (void)sh_chmod(argv[2], 0600);
        if (sh_write_file(argv[2], data, (uint64_t)n) < 0) {
            mem_wipe(data, sizeof(data));
            return fail(out, cap, "ws: you cannot write there\n");
        }
        mem_wipe(data, sizeof(data));
        str_copy(out, "saved a copy to ", cap);
        str_cat(out, argv[2], cap);
        str_cat(out, "\n", cap);
        return (long)str_len(out);
    }
    return ws_request(cmdline, 0, 0, out, cap, 0);
}
