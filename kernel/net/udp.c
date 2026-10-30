#include "udp.h"
#include "net.h"
#include "../drivers/net/net_drv.h"
#include "../proc/sched.h"

#define ARP_CACHE   32
#define BINDINGS    16
#define QUEUE_LEN   16
#define MAX_DGRAM   (UDP_HDR_IN + UDP_MAX_PAYLOAD)

typedef struct {
    uint32_t ip;
    uint8_t mac[6];
    int used;
} arp_entry_t;

typedef struct {
    uint16_t len;
    uint8_t data[MAX_DGRAM];
} dgram_t;

typedef struct {
    int used;
    uint16_t port;
    uint64_t pid;
    uint32_t head, tail, count;
    dgram_t q[QUEUE_LEN];
} binding_t;

static arp_entry_t arp_cache[ARP_CACHE];
static uint32_t arp_next;
static binding_t bindings[BINDINGS];
static const uint8_t BCAST[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };

/* The NIC and these tables are touched from syscall context only; keep
 * interrupts off around them so a preempting process cannot interleave. */
static uint64_t irq_save(void) {
    uint64_t flags;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(flags) : : "memory");
    return flags;
}

static void irq_restore(uint64_t flags) {
    if (flags & (1u << 9)) __asm__ volatile("sti" : : : "memory");
}

static uint32_t ip_of(const uint8_t b[4]) {
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

static int is_broadcast(uint32_t ip) {
    return ip == 0xffffffffu || (ip | net_state.netmask) == 0xffffffffu;
}

static void arp_learn(uint32_t ip, const uint8_t mac[6]) {
    arp_entry_t *slot = 0;
    if (!ip || ip == net_state.ip) return;
    for (int i = 0; i < ARP_CACHE; i++) {
        if (arp_cache[i].used && arp_cache[i].ip == ip) {
            slot = &arp_cache[i];
            break;
        }
    }
    if (!slot) {
        slot = &arp_cache[arp_next];
        arp_next = (arp_next + 1) % ARP_CACHE;
    }
    slot->used = 1;
    slot->ip = ip;
    copy_bytes(slot->mac, mac, 6);
}

static int arp_lookup(uint32_t ip, uint8_t mac[6]) {
    for (int i = 0; i < ARP_CACHE; i++) {
        if (arp_cache[i].used && arp_cache[i].ip == ip) {
            copy_bytes(mac, arp_cache[i].mac, 6);
            return 1;
        }
    }
    return 0;
}

static void send_arp(uint16_t oper, const uint8_t dst_mac[6], const uint8_t tha[6], uint32_t tpa) {
    uint8_t frame[64];
    eth_hdr_t *eth = (eth_hdr_t *)frame;
    arp_pkt_t *arp = (arp_pkt_t *)(frame + sizeof(eth_hdr_t));
    zero_bytes(frame, sizeof(frame));
    build_eth(eth, dst_mac, ETH_TYPE_ARP);
    arp->htype_be = htons16(ARP_HTYPE_ETHERNET);
    arp->ptype_be = htons16(ARP_PTYPE_IPV4);
    arp->hlen = 6;
    arp->plen = 4;
    arp->oper_be = htons16(oper);
    copy_bytes(arp->sha, net_state.mac, 6);
    ip_to_bytes(net_state.ip, arp->spa);
    copy_bytes(arp->tha, tha, 6);
    ip_to_bytes(tpa, arp->tpa);
    (void)net_drv_send_frame(frame, (uint16_t)(sizeof(eth_hdr_t) + sizeof(arp_pkt_t)));
}

static int pid_alive(uint64_t pid) {
    process_t *p = sched_find_process(pid);
    return p && p->state != PROCESS_EXITED;
}

static void deliver(uint32_t src_ip, uint16_t src_port, uint16_t dst_port, const uint8_t *data, uint16_t len) {
    for (int i = 0; i < BINDINGS; i++) {
        binding_t *b = &bindings[i];
        dgram_t *d;
        if (!b->used || b->port != dst_port) continue;
        if (b->count == QUEUE_LEN || len > UDP_MAX_PAYLOAD) return; /* full: drop, like a real socket */
        d = &b->q[b->head];
        ip_to_bytes(src_ip, d->data);
        d->data[4] = (uint8_t)src_port;
        d->data[5] = (uint8_t)(src_port >> 8);
        d->data[6] = (uint8_t)dst_port;
        d->data[7] = (uint8_t)(dst_port >> 8);
        d->data[8] = (uint8_t)len;
        d->data[9] = (uint8_t)(len >> 8);
        d->data[10] = d->data[11] = 0;
        copy_bytes(d->data + UDP_HDR_IN, data, len);
        d->len = (uint16_t)(UDP_HDR_IN + len);
        b->head = (b->head + 1) % QUEUE_LEN;
        b->count++;
        return;
    }
}

static void handle_frame(const uint8_t *frame, uint16_t len) {
    const eth_hdr_t *eth = (const eth_hdr_t *)frame;
    uint16_t type;

    if (len < sizeof(eth_hdr_t)) return;
    type = ntohs16(eth->type_be);
    if (type == ETH_TYPE_ARP && len >= sizeof(eth_hdr_t) + sizeof(arp_pkt_t)) {
        const arp_pkt_t *arp = (const arp_pkt_t *)(frame + sizeof(eth_hdr_t));
        uint32_t spa = ip_of(arp->spa), tpa = ip_of(arp->tpa);
        arp_learn(spa, arp->sha);
        if (ntohs16(arp->oper_be) == ARP_OP_REQUEST && tpa == net_state.ip) {
            send_arp(ARP_OP_REPLY, arp->sha, arp->sha, spa);
        }
        return;
    }
    if (type == ETH_TYPE_IPV4 && len >= sizeof(eth_hdr_t) + sizeof(ipv4_hdr_t)) {
        const ipv4_hdr_t *ip = (const ipv4_hdr_t *)(frame + sizeof(eth_hdr_t));
        uint16_t ihl = (uint16_t)((ip->ver_ihl & 0x0f) * 4), total = ntohs16(ip->total_len_be);
        const udp_hdr_t *udp;
        uint16_t ulen;

        if ((ip->ver_ihl >> 4) != 4 || ihl < sizeof(ipv4_hdr_t)) return;
        if (sizeof(eth_hdr_t) + total > len || ihl + sizeof(udp_hdr_t) > total) return;
        if (ip->src_be == net_state.ip) return;            /* our own broadcast echoed back */
        if (ip->dst_be != net_state.ip && !is_broadcast(ip->dst_be)) return;
        if (ntohs16(ip->frag_be) & 0x3fff) return;          /* no fragment reassembly */
        arp_learn(ip->src_be, eth->src);
        if (ip->proto != IP_PROTO_UDP) return;
        udp = (const udp_hdr_t *)((const uint8_t *)ip + ihl);
        ulen = ntohs16(udp->len_be);
        if (ulen < sizeof(udp_hdr_t) || ihl + ulen > total) return;
        deliver(ip->src_be, ntohs16(udp->src_port_be), ntohs16(udp->dst_port_be),
                (const uint8_t *)udp + sizeof(udp_hdr_t), (uint16_t)(ulen - sizeof(udp_hdr_t)));
    }
}

void net_poll(void) {
    static uint8_t frame[NET_FRAME_CAP];
    uint64_t flags;
    if (!net_ready()) return;
    flags = irq_save();
    for (int n = 0; n < 64; n++) {
        uint16_t len = 0;
        if (net_drv_recv_frame(frame, sizeof(frame), &len) <= 0) break;
        handle_frame(frame, len);
    }
    irq_restore(flags);
}

static binding_t *find_binding(uint16_t port) {
    for (int i = 0; i < BINDINGS; i++) {
        if (bindings[i].used && bindings[i].port == port) return &bindings[i];
    }
    return 0;
}

int udp_bind(uint64_t pid, uint16_t port) {
    binding_t *b;
    uint64_t flags;
    int r = -1;
    if (!port) return -1;
    flags = irq_save();
    b = find_binding(port);
    if (b && !pid_alive(b->pid)) b->used = 0; /* reclaim from an exited process */
    if (!find_binding(port)) {
        for (int i = 0; i < BINDINGS; i++) {
            if (bindings[i].used && !pid_alive(bindings[i].pid)) bindings[i].used = 0;
            if (!bindings[i].used) {
                b = &bindings[i];
                b->used = 1;
                b->port = port;
                b->pid = pid;
                b->head = b->tail = b->count = 0;
                r = 0;
                break;
            }
        }
    } else if (find_binding(port)->pid == pid) {
        r = 0;
    }
    irq_restore(flags);
    return r;
}

int udp_unbind(uint64_t pid, uint16_t port) {
    binding_t *b = find_binding(port);
    if (!b || b->pid != pid) return -1;
    b->used = 0;
    return 0;
}

/* Resolve a next-hop MAC, polling for the reply for up to ~0.5 s. */
static int resolve(uint32_t ip, uint8_t mac[6]) {
    uint32_t hop = ip_same_subnet(ip, net_state.ip, net_state.netmask) ? ip : net_state.gateway;
    static const uint8_t zero[6];
    if (is_broadcast(ip)) {
        copy_bytes(mac, BCAST, 6);
        return 0;
    }
    for (int attempt = 0; attempt < 3; attempt++) {
        uint64_t flags = irq_save(), deadline;
        int found = arp_lookup(hop, mac);
        if (!found) send_arp(ARP_OP_REQUEST, BCAST, zero, hop);
        irq_restore(flags);
        if (found) return 0;
        deadline = sched_ticks() + 15;
        while (sched_ticks() < deadline) {
            net_poll();
            if (arp_lookup(hop, mac)) return 0;
            sched_sleep(1);
        }
    }
    return -1;
}

int udp_send(uint64_t pid, uint16_t src_port, const uint8_t dst_ip[4], uint16_t dst_port,
             const uint8_t *data, uint16_t len) {
    binding_t *b = find_binding(src_port);
    uint8_t mac[6];
    uint64_t flags;
    int r;
    if (!net_ready() || !b || b->pid != pid || len > UDP_MAX_PAYLOAD || !dst_port) return -1;
    if (resolve(ip_of(dst_ip), mac) != 0) return -1;
    flags = irq_save();
    r = send_udp_packet(mac, ip_of(dst_ip), src_port, dst_port, data, len);
    irq_restore(flags);
    net_poll();
    return r;
}

int udp_recv(uint64_t pid, uint8_t *out, uint64_t cap) {
    uint64_t flags;
    int r = 0;
    net_poll();
    flags = irq_save();
    for (int i = 0; i < BINDINGS; i++) {
        binding_t *b = &bindings[i];
        dgram_t *d;
        if (!b->used || b->pid != pid || !b->count) continue;
        d = &b->q[b->tail];
        if (d->len > cap) {
            r = -1;
            break;
        }
        copy_bytes(out, d->data, d->len);
        r = d->len;
        b->tail = (b->tail + 1) % QUEUE_LEN;
        b->count--;
        break;
    }
    irq_restore(flags);
    return r;
}

static uint64_t put_ip(char *out, uint64_t o, uint64_t cap, uint32_t ip) {
    uint8_t b[4];
    ip_to_bytes(ip, b);
    for (int i = 0; i < 4; i++) {
        char t[4];
        int n = 0;
        uint32_t v = b[i];
        do {
            t[n++] = (char)('0' + v % 10);
            v /= 10;
        } while (v);
        while (n && o + 1 < cap) out[o++] = t[--n];
        if (i < 3 && o + 1 < cap) out[o++] = '.';
    }
    return o;
}

static uint64_t put_str(char *out, uint64_t o, uint64_t cap, const char *s) {
    while (*s && o + 1 < cap) out[o++] = *s++;
    return o;
}

uint64_t net_info_text(char *out, uint64_t cap) {
    static const char hex[] = "0123456789abcdef";
    uint64_t o = 0;
    if (!cap) return 0;
    if (!net_ready()) {
        o = put_str(out, o, cap, "offline\n");
    } else {
        o = put_str(out, o, cap, "ip ");
        o = put_ip(out, o, cap, net_state.ip);
        o = put_str(out, o, cap, "\nmask ");
        o = put_ip(out, o, cap, net_state.netmask);
        o = put_str(out, o, cap, "\nmac ");
        for (int i = 0; i < 6 && o + 3 < cap; i++) {
            out[o++] = hex[net_state.mac[i] >> 4];
            out[o++] = hex[net_state.mac[i] & 15];
            if (i < 5) out[o++] = ':';
        }
        o = put_str(out, o, cap, "\n");
    }
    out[o] = '\0';
    return o;
}
