/* Client side of the workspaced control protocol (wsproto.h), shared by
 * the `ws` command and the Terminal's built-in `ws`. */
#include "lib.h"
#include "wsclient.h"
#include "wsproto.h"

static long fail(char *out, uint64_t cap, const char *why) {
    str_copy(out, why, cap);
    return -1;
}

long ws_call(const char *line, char *out, uint64_t cap) {
    char n[24], in_path[64], out_path[64], box[48];
    uint64_t ctl, me, end;
    ws_ctl_t m;
    long len, owner;

    fmt_u64(n, sh_pid(), 10);
    str_copy(in_path, WS_IO_DIR "/", sizeof(in_path));
    str_cat(in_path, n, sizeof(in_path));
    str_copy(out_path, in_path, sizeof(out_path));
    str_cat(in_path, ".in", sizeof(in_path));
    str_cat(out_path, ".out", sizeof(out_path));
    if (sh_write_file(in_path, line, str_len(line) + 1) < 0) {
        return fail(out, cap, "ws: the workspace service is not running\n");
    }

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
    len = sh_read(out_path, out, cap - 1);
    if (len < 0) len = 0;
    out[len] = '\0';
    return len;
}
