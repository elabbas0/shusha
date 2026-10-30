#ifndef KERNEL_NET_UDP_H
#define KERNEL_NET_UDP_H

#include <stdint.h>

/*
 * Userspace UDP for Shusha's services (workspace, discovery).
 *
 * The NIC is polled: net_poll() drains received frames, answers ARP
 * requests for our address, learns neighbours' MACs, and queues UDP
 * datagrams for the process that bound the destination port.  It runs
 * whenever a process sends or receives, so an active service keeps the
 * machine reachable.
 *
 * Exposed through /dev/udp (no new syscall numbers):
 *   write  op 1 BIND   [op u8][pad u8][port u16]
 *          op 2 UNBIND [op u8][pad u8][port u16]
 *          op 3 SEND   [op u8][pad u8][src_port u16][dst_port u16][pad u16]
 *                      [dst_ip 4 bytes a.b.c.d][payload...]
 *   read_at(offset 0)  next datagram for any port the caller bound:
 *                      [src_ip 4][src_port u16][dst_port u16][len u16][pad u16]
 *                      [payload...]; 0 when nothing is waiting.
 * /dev/netinfo reads as text: "ip a.b.c.d\nmask a.b.c.d\nmac ..\n".
 * Ports are little-endian.  dst 255.255.255.255 or the subnet broadcast
 * address goes to every machine on the LAN.
 */

#define UDP_OP_BIND   1
#define UDP_OP_UNBIND 2
#define UDP_OP_SEND   3
#define UDP_HDR_IN    12      /* bytes before the payload on read */
#define UDP_MAX_PAYLOAD 1400

void     net_poll(void);
int      udp_bind(uint64_t pid, uint16_t port);
int      udp_unbind(uint64_t pid, uint16_t port);
int      udp_send(uint64_t pid, uint16_t src_port, const uint8_t dst_ip[4], uint16_t dst_port,
                  const uint8_t *data, uint16_t len);
/* Copies the next datagram (header + payload) into out; returns its
 * total length, 0 if none, -1 if it does not fit. */
int      udp_recv(uint64_t pid, uint8_t *out, uint64_t cap);
/* Text for /dev/netinfo. */
uint64_t net_info_text(char *out, uint64_t cap);

#endif
