#ifndef SHUSHA_WSPROTO_H
#define SHUSHA_WSPROTO_H

/* Local control interface between the `ws` command and workspaced.
 * The command writes its command line to /var/ws/io/<pid>.in, sends a
 * WS_CTL_RUN message to WS_CTL_QUEUE, and waits for WS_CTL_DONE on its
 * private mailbox /proc/<pid>/ws; the output is in /var/ws/io/<pid>.out.
 * The daemon checks the kernel-stamped sender pid against the message;
 * the command checks the reply came from the owner of WS_CTL_QUEUE. */

#include <stdint.h>

#define WS_CTL_QUEUE "/ws/ctl"
#define WS_DIR       "/var/ws"
#define WS_IO_DIR    "/var/ws/io"
#define WS_PORT      7100

enum { WS_CTL_RUN = 1, WS_CTL_DONE = 2 };

typedef struct {
    uint32_t type;
    uint32_t pid;
    int32_t  status;
    uint8_t  pad[52];
} ws_ctl_t;

typedef char ws_ctl_size_check[sizeof(ws_ctl_t) == 64 ? 1 : -1];

#endif
