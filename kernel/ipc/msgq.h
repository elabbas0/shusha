#ifndef IPC_MSGQ_H
#define IPC_MSGQ_H

#include <stdint.h>

#define MSGQ_MAX_QUEUES 32
#define MSGQ_MAX_MSGS   64
#define MSGQ_MSG_SIZE   64
#define MSGQ_NAME_MAX   64

/* Returns 1-based handle, 0 on failure */
uint64_t msgq_open(const char *name);

/* Enqueue a MSGQ_MSG_SIZE-byte message. Returns 0 ok, -1 full/bad */
int msgq_send(uint64_t handle, const void *msg);

/* Owner only.  flags: MSGQ_RECV_BLOCK waits for a message;
 * MSGQ_RECV_SENDER returns the sender's pid instead of 0.  -1 when
 * empty, not the owner, or a bad handle. */
#define MSGQ_RECV_BLOCK  1
#define MSGQ_RECV_SENDER 2
int64_t msgq_recv(uint64_t handle, void *out, int flags);

/* 1 if a message is waiting, 0 if empty, -1 bad handle or not the owner. */
/* With MSGQ_POLL_OWNER: the owning pid (anyone may ask). */
#define MSGQ_POLL_OWNER 1
int64_t msgq_poll(uint64_t handle, int flags);

/* Future ref-counting stub */
void msgq_close(uint64_t handle);

#endif
