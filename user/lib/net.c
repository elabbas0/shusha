#include "net.h"
#include "lib.h"

static uint8_t pkt[12 + NET_MAX_PAYLOAD];

int net_parse_ip(const char *t, uint8_t ip[4]) {
    for (int i = 0; i < 4; i++) {
        uint32_t v = 0;
        int digits = 0;
        while (*t >= '0' && *t <= '9') {
            v = v * 10 + (uint32_t)(*t++ - '0');
            digits++;
        }
        if (!digits || v > 255) return -1;
        ip[i] = (uint8_t)v;
        if (i < 3 && *t++ != '.') return -1;
    }
    return 0;
}

char *net_ip_text(char *out, const uint8_t ip[4]) {
    char n[8];
    out[0] = '\0';
    for (int i = 0; i < 4; i++) {
        str_cat(out, fmt_u64(n, ip[i], 10), 16);
        if (i < 3) str_cat(out, ".", 16);
    }
    return out;
}

int net_local(uint8_t ip[4], uint8_t mask[4]) {
    char info[128];
    char *p;
    if (sh_read("/dev/netinfo", info, sizeof(info)) <= 0 || !str_starts(info, "ip ")) return -1;
    if (net_parse_ip(info + 3, ip) != 0) return -1;
    for (p = info; *p && !str_starts(p, "mask "); p++) {}
    return *p && net_parse_ip(p + 5, mask) == 0 ? 0 : -1;
}

void net_broadcast_addr(uint8_t out[4]) {
    uint8_t ip[4], mask[4];
    if (net_local(ip, mask) != 0) {
        for (int i = 0; i < 4; i++) out[i] = 255;
        return;
    }
    for (int i = 0; i < 4; i++) out[i] = (uint8_t)(ip[i] | (uint8_t)~mask[i]);
}

int net_bind(uint16_t port) {
    uint8_t cmd[4] = { 1, 0, (uint8_t)port, (uint8_t)(port >> 8) };
    return sh_write_file("/dev/udp", cmd, 4) == 4 ? 0 : -1;
}

int net_send(uint16_t from_port, const uint8_t ip[4], uint16_t port, const void *data, uint64_t len) {
    if (len > NET_MAX_PAYLOAD) return -1;
    pkt[0] = 3;
    pkt[1] = 0;
    pkt[2] = (uint8_t)from_port;
    pkt[3] = (uint8_t)(from_port >> 8);
    pkt[4] = (uint8_t)port;
    pkt[5] = (uint8_t)(port >> 8);
    pkt[6] = pkt[7] = 0;
    mem_copy(pkt + 8, ip, 4);
    mem_copy(pkt + 12, data, len);
    return sh_write_file("/dev/udp", pkt, 12 + len) == (long)(12 + len) ? 0 : -1;
}

long net_recv(void *buf, uint64_t cap, net_addr_t *from, uint16_t *to_port) {
    long n = sh_read_at("/dev/udp", 0, (char *)pkt, sizeof(pkt));
    uint16_t len;
    if (n <= 0) return n;
    len = (uint16_t)(pkt[8] | (pkt[9] << 8));
    if (len > cap) return -1;
    if (from) {
        mem_copy(from->ip, pkt, 4);
        from->port = (uint16_t)(pkt[4] | (pkt[5] << 8));
    }
    if (to_port) *to_port = (uint16_t)(pkt[6] | (pkt[7] << 8));
    mem_copy(buf, pkt + 12, len);
    return len;
}
