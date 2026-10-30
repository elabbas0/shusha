/* ws: command-line front end for workspaced (see wsproto.h).
 *
 *   ws status | create <name> | invite | join <code> [name] | members
 *   ws put <file> | files | cat <file> | request <file> | requests
 *   ws grant <file> <rank> | sync
 */
#include "lib.h"
#include "wsproto.h"

static char line[256], outbuf[8192];

int main(int argc, char **argv) {
    char n[24], in_path[64], out_path[64], box[48];
    uint64_t ctl, me;
    ws_ctl_t m;
    uint64_t end;
    long len, owner;

    line[0] = '\0';
    for (int i = 1; i < argc; i++) {
        if (i > 1) str_cat(line, " ", sizeof(line));
        str_cat(line, argv[i], sizeof(line));
    }
    fmt_u64(n, sh_pid(), 10);
    str_copy(in_path, WS_IO_DIR "/", sizeof(in_path));
    str_cat(in_path, n, sizeof(in_path));
    str_copy(out_path, in_path, sizeof(out_path));
    str_cat(in_path, ".in", sizeof(in_path));
    str_cat(out_path, ".out", sizeof(out_path));
    if (sh_write_file(in_path, line, str_len(line) + 1) < 0) {
        print("ws: the workspace service is not running\n");
        return 1;
    }

    str_copy(box, "/proc/", sizeof(box));
    str_cat(box, n, sizeof(box));
    str_cat(box, "/ws", sizeof(box));
    me = sh_msg_open(box);
    ctl = sh_msg_open(WS_CTL_QUEUE);
    owner = ctl ? sh_msg_owner(ctl) : -1;
    if (!me || owner <= 0 || owner == (long)sh_pid()) {
        print("ws: the workspace service is not running\n");
        return 1;
    }
    mem_set(&m, 0, sizeof(m));
    m.type = WS_CTL_RUN;
    m.pid = (uint32_t)sh_pid();
    if (sh_msg_send(ctl, &m) != 0) {
        print("ws: cannot reach the workspace service\n");
        return 1;
    }
    /* Joins and syncs talk to other machines; allow a minute. */
    end = sh_ticks() + 6000;
    for (;;) {
        long from = sh_msg_recv_from(me, &m);
        if (from == owner && m.type == WS_CTL_DONE && m.pid == (uint32_t)sh_pid()) break;
        if (sh_ticks() > end) {
            print("ws: timed out waiting for the workspace service\n");
            return 1;
        }
        if (from <= 0) sh_sleep(2);
    }
    len = sh_read(out_path, outbuf, sizeof(outbuf) - 1);
    if (len > 0) {
        outbuf[len] = '\0';
        print(outbuf);
    }
    return m.status;
}
