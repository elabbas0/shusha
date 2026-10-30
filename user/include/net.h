#ifndef SHUSHA_NET_H
#define SHUSHA_NET_H

/* UDP for userspace services, over the kernel's /dev/udp. */

#include <stdint.h>

#define NET_MAX_PAYLOAD 1400

typedef struct {
    uint8_t ip[4];
    uint16_t port;
} net_addr_t;

/* Our IPv4 address and netmask; -1 when the network is down. */
int  net_local(uint8_t ip[4], uint8_t mask[4]);
void net_broadcast_addr(uint8_t out[4]);
int  net_bind(uint16_t port);
int  net_send(uint16_t from_port, const uint8_t ip[4], uint16_t port, const void *data, uint64_t len);
/* Next datagram: payload length (0 if none waiting, -1 on error). */
long net_recv(void *buf, uint64_t cap, net_addr_t *from, uint16_t *to_port);
/* "a.b.c.d" <-> bytes */
char *net_ip_text(char *out, const uint8_t ip[4]);
int  net_parse_ip(const char *text, uint8_t ip[4]);

#endif
