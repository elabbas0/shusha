/* Shared request/answer plumbing for system services (see svc.h). */
#include "lib.h"
#include "svc.h"

static void io_path(char *out, uint64_t cap, const char *io_dir, uint64_t pid, const char *ext) {
    char n[24];
    str_copy(out, io_dir, cap);
    str_cat(out, "/", cap);
    str_cat(out, fmt_u64(n, pid, 10), cap);
    str_cat(out, ext, cap);
}

static void box_path(char *out, uint64_t cap, uint64_t pid, const char *box) {
    char n[24];
    str_copy(out, "/proc/", cap);
    str_cat(out, fmt_u64(n, pid, 10), cap);
    str_cat(out, "/", cap);
    str_cat(out, box, cap);
}

static long fail(char *out, uint64_t cap, const char *why) {
    str_copy(out, why, cap);
    return -1;
}

long svc_request(const char *queue, const char *io_dir, const char *box, const char *line,
                 const void *data, uint64_t dlen, char *out, uint64_t cap, int *status) {
    static uint8_t req[256 + 32768];
    char in_path[80], out_path[80], mbox[48];
    uint64_t ctl, me, end, len = str_len(line) + 1;
    svc_ctl_t m;
    long got, owner;

    if (status) *status = 0;
    if (len > 256 || dlen > sizeof(req) - 256) return fail(out, cap, "request too large\n");
    mem_copy(req, line, len);
    if (data) mem_copy(req + len, data, dlen);
    io_path(in_path, sizeof(in_path), io_dir, sh_pid(), ".in");
    io_path(out_path, sizeof(out_path), io_dir, sh_pid(), ".out");
    /* Private before it holds anything. */
    (void)sh_create(in_path);
    (void)sh_chmod(in_path, 0600);
    if (sh_write_file(in_path, req, len + (data ? dlen : 0)) < 0) {
        mem_wipe(req, len + dlen);
        return fail(out, cap, "the service is not running\n");
    }
    mem_wipe(req, len + dlen);

    box_path(mbox, sizeof(mbox), sh_pid(), box);
    me = sh_msg_open(mbox);
    ctl = sh_msg_open(queue);
    owner = ctl ? sh_msg_owner(ctl) : -1;
    if (!me || owner <= 0 || owner == (long)sh_pid()) {
        (void)sh_write_file(in_path, "", 0);
        return fail(out, cap, "the service is not running\n");
    }
    mem_set(&m, 0, sizeof(m));
    m.type = SVC_RUN;
    m.pid = (uint32_t)sh_pid();
    if (sh_msg_send(ctl, &m) != 0) return fail(out, cap, "cannot reach the service\n");
    end = sh_ticks() + 6000;
    for (;;) {
        long from = sh_msg_recv_from(me, &m);
        if (from == owner && m.type == SVC_DONE && m.pid == (uint32_t)sh_pid()) break;
        if (sh_ticks() > end) return fail(out, cap, "timed out waiting for the service\n");
        if (from <= 0) sh_sleep(2);
    }
    got = sh_read(out_path, out, cap - 1);
    if (got < 0) got = 0;
    out[got] = '\0';
    (void)sh_write_file(out_path, "", 0);
    if (status) *status = m.status;
    return got;
}

int svc_next(uint64_t ctl, const char *io_dir, const char *box, uint8_t *buf, uint64_t cap, svc_req_t *req) {
    char in_path[80];
    sh_stat_t st;
    svc_ctl_t m;
    long cred = sh_msg_recv_cred(ctl, &m), len;
    uint64_t nl = 0;

    if (cred <= 0) return 0;
    req->pid = (uint32_t)((uint64_t)cred & 0xffffffffu);
    req->uid = (uint32_t)((uint64_t)cred >> 32);
    req->data = 0;
    req->dlen = 0;
    req->line[0] = '\0';
    /* Honoured only from the process it names. */
    if (m.type != SVC_RUN || m.pid != req->pid) return -1;
    io_path(in_path, sizeof(in_path), io_dir, req->pid, ".in");
    len = sh_stat(in_path, &st) == 0 && st.uid == req->uid && !(st.mode & 0002)
              ? sh_read(in_path, (char *)buf, cap - 1) : -1;
    (void)sh_write_file(in_path, "", 0);
    if (len < 0) {
        static const char refused[] = "request refused (the request file is not yours alone)\n";
        svc_reply(io_dir, box, req, refused, sizeof(refused) - 1, 1);
        return -1;
    }
    buf[len] = 0;
    while (nl < (uint64_t)len && buf[nl]) nl++;
    str_copy(req->line, (char *)buf, sizeof(req->line));
    if (nl + 1 <= (uint64_t)len) {
        req->data = buf + nl + 1;
        req->dlen = (uint64_t)len - nl - 1;
    }
    return 1;
}

void svc_reply(const char *io_dir, const char *box, const svc_req_t *req, const void *answer, uint64_t n,
               int status) {
    char out_path[80], mbox[48];
    svc_ctl_t done;
    uint64_t q;
    io_path(out_path, sizeof(out_path), io_dir, req->pid, ".out");
    /* The answer may be sensitive: the requester's, private, then filled. */
    (void)sh_write_file(out_path, "", 0);
    (void)sh_chown(out_path, req->uid);
    (void)sh_chmod(out_path, 0600);
    (void)sh_write_file(out_path, answer, n);
    mem_set(&done, 0, sizeof(done));
    done.type = SVC_DONE;
    done.pid = req->pid;
    done.status = status;
    box_path(mbox, sizeof(mbox), req->pid, box);
    q = sh_msg_open(mbox);
    if (q) (void)sh_msg_send(q, &done);
}
