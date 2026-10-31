#ifndef SHUSHA_SVC_H
#define SHUSHA_SVC_H

/* Local request/answer protocol for Shusha's system services (workspaced,
 * authd).  A client writes "<command line>\0<data>" to <io_dir>/<pid>.in
 * (its own file, mode 0600), sends SVC_RUN with its pid to the service's
 * queue and waits on /proc/<pid>/<box> for SVC_DONE from the queue's
 * owner; the answer is in <io_dir>/<pid>.out, which the service makes the
 * client's and private before writing.  The service trusts only the
 * kernel-stamped pid and uid of the message, and only a request file the
 * client owns and others cannot write. */

#include <stdint.h>

enum { SVC_RUN = 1, SVC_DONE = 2 };

typedef struct {
    uint32_t type;
    uint32_t pid;
    int32_t  status;
    uint8_t  pad[52];
} svc_ctl_t;

typedef char svc_ctl_size_check[sizeof(svc_ctl_t) == 64 ? 1 : -1];

/* ---- client ---- */
/* Returns the answer's length (answer NUL-terminated in out), or -1 with a
 * reason in out.  *status (if given) receives the service's status. */
long svc_request(const char *queue, const char *io_dir, const char *box, const char *line,
                 const void *data, uint64_t dlen, char *out, uint64_t cap, int *status);

/* ---- service ---- */
typedef struct {
    uint32_t pid, uid;
    char line[256];
    uint8_t *data;      /* points into the service's buffer */
    uint64_t dlen;
} svc_req_t;

/* Next request on ctl, read into buf (cap bytes).  Returns 1 with *req
 * filled, 0 if none waiting, -1 for a request that was refused (already
 * answered). */
int  svc_next(uint64_t ctl, const char *io_dir, const char *box, uint8_t *buf, uint64_t cap, svc_req_t *req);
void svc_reply(const char *io_dir, const char *box, const svc_req_t *req, const void *answer, uint64_t n,
               int status);

#endif
