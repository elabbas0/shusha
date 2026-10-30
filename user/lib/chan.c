#include "chan.h"
#include "lib.h"
#include "aead.h"
#include "ed25519.h"
#include "mlkem.h"
#include "sha2.h"
#include "sc.h"

enum { HS1 = 1, HS2, HS3, DATA };
enum { ST_FREE, ST_SENT_HS1, ST_SENT_HS2, ST_OPEN };

typedef struct {
    int state;
    net_addr_t peer;
    uint8_t sid_local[8], sid_peer[8];
    uint8_t e_priv[32], e_pub[32];
    uint8_t kem_ek[SC_MLKEM_EK], kem_dk[SC_MLKEM_DK]; /* initiator's ephemeral */
    uint8_t th[32];       /* running transcript hash */
    uint8_t prk[32];
    uint8_t k_send[32], k_recv[32];
    uint64_t send_ctr, recv_max, recv_bits;
    uint8_t peer_pub[32];
    uint8_t hs1[1 + 8 + 32 + SC_MLKEM_EK]; /* kept for retransmission */
} chan_t;

static chan_t chans[CHAN_MAX];
static uint8_t my_seed[32], my_pub[32];
static uint16_t my_port;
static uint8_t pkt[NET_MAX_PAYLOAD];

/* Messages that arrived while chan_connect was waiting. */
#define BACKLOG 8
static chan_msg_t backlog[BACKLOG];
static int backlog_n;

void rand_bytes(void *buf, uint64_t len) {
    uint8_t *p = buf;
    while (len) {
        uint64_t n = len > 256 ? 256 : len;
        if (sh_read_at("/dev/random", 0, (char *)p, n) != (long)n) {
            print("chan: /dev/random unavailable\n");
            sh_exit(1);
        }
        p += n;
        len -= n;
    }
}

int chan_init(const uint8_t sign_seed[32], const uint8_t sign_pub[32], uint16_t port) {
    mem_copy(my_seed, sign_seed, 32);
    mem_copy(my_pub, sign_pub, 32);
    my_port = port;
    return net_bind(port);
}

const uint8_t *chan_peer(int c) {
    return c >= 0 && c < CHAN_MAX && chans[c].state == ST_OPEN ? chans[c].peer_pub : 0;
}

void chan_close(int c) {
    if (c >= 0 && c < CHAN_MAX) mem_wipe(&chans[c], sizeof(chans[c]));
}

static void hash2(uint8_t out[32], const uint8_t *a, uint64_t alen, const uint8_t *b, uint64_t blen) {
    sc_sha256_t h;
    sc_sha256_init(&h);
    sc_sha256_update(&h, a, alen);
    sc_sha256_update(&h, b, blen);
    sc_sha256_final(&h, out);
}

static void expand(uint8_t out[32], const uint8_t prk[32], const char *label, const uint8_t th[32]) {
    uint8_t info[64];
    uint64_t n = str_len(label);
    mem_copy(info, label, n);
    if (th) {
        mem_copy(info + n, th, 32);
        n += 32;
    }
    sc_hkdf_expand(prk, info, n, out, 32);
}

static void nonce_for(uint8_t n[12], uint64_t ctr) {
    mem_set(n, 0, 12);
    sc_store64_le(n + 4, ctr);
}

static int find_local_sid(const uint8_t sid[8]) {
    for (int i = 0; i < CHAN_MAX; i++) {
        if (chans[i].state != ST_FREE && mem_eq_ct(chans[i].sid_local, sid, 8)) return i;
    }
    return -1;
}

static int alloc_chan(void) {
    for (int i = 0; i < CHAN_MAX; i++) {
        if (chans[i].state == ST_FREE) return i;
    }
    /* Table full: drop the oldest half-open handshake, then slot 0. */
    for (int i = 0; i < CHAN_MAX; i++) {
        if (chans[i].state != ST_OPEN) {
            chan_close(i);
            return i;
        }
    }
    chan_close(0);
    return 0;
}

/* Signed identity block: pub(32) || Sign(label || th)(64), sealed under k. */
static void seal_identity(uint8_t out[112], const uint8_t k[32], const char *label, const uint8_t th[32]) {
    uint8_t msg[64], blk[96], zero[12] = {0};
    uint64_t n = str_len(label);
    mem_copy(msg, label, n);
    mem_copy(msg + n, th, 32);
    mem_copy(blk, my_pub, 32);
    sc_ed25519_sign(blk + 32, msg, n + 32, my_seed, my_pub);
    sc_aead_seal(out, out + 96, blk, 96, th, 32, zero, k);
}

static int open_identity(uint8_t pub[32], const uint8_t in[112], const uint8_t k[32], const char *label,
                         const uint8_t th[32]) {
    uint8_t blk[96], msg[64], zero[12] = {0};
    uint64_t n = str_len(label);
    if (sc_aead_open(blk, in, 96, in + 96, th, 32, zero, k) != 0) return -1;
    mem_copy(msg, label, n);
    mem_copy(msg + n, th, 32);
    if (sc_ed25519_verify(blk + 32, msg, n + 32, blk) != 0) return -1;
    mem_copy(pub, blk, 32);
    return 0;
}

/* ---- handshake handlers ------------------------------------------------ */

static void on_hs1(const uint8_t *p, long n, const net_addr_t *from) {
    uint8_t out[1 + 8 + 8 + 32 + SC_MLKEM_CT + 112], ss[32], m[32], ikm[64], k_hs_r[32];
    chan_t *c;
    int i;
    if (n != 1 + 8 + 32 + SC_MLKEM_EK) return;
    /* A retransmitted HS1 for a handshake we already answered: answer again. */
    for (i = 0; i < CHAN_MAX; i++) {
        if (chans[i].state == ST_SENT_HS2 && mem_eq_ct(chans[i].sid_peer, p + 1, 8) &&
            mem_eq_ct(chans[i].peer.ip, from->ip, 4)) {
            chan_close(i);
            break;
        }
    }
    i = alloc_chan();
    c = &chans[i];
    c->state = ST_SENT_HS2;
    c->peer = *from;
    mem_copy(c->sid_peer, p + 1, 8);
    rand_bytes(c->sid_local, 8);
    rand_bytes(c->e_priv, 32);
    sc_x25519_base(c->e_pub, c->e_priv);
    rand_bytes(m, 32);

    out[0] = HS2;
    mem_copy(out + 1, c->sid_peer, 8);
    mem_copy(out + 9, c->sid_local, 8);
    mem_copy(out + 17, c->e_pub, 32);
    if (sc_mlkem768_encaps(out + 49, ss, p + 41, m) != 0) {
        chan_close(i);
        return;
    }
    sc_x25519(ikm, c->e_priv, p + 9);
    mem_copy(ikm + 32, ss, 32);
    /* th1 = H(label || HS1 || sid_r || eR || ct) */
    {
        sc_sha256_t h;
        static const char lab[] = "shusha-ch-v1";
        sc_sha256_init(&h);
        sc_sha256_update(&h, (const uint8_t *)lab, sizeof(lab) - 1);
        sc_sha256_update(&h, p, (uint64_t)n);
        sc_sha256_update(&h, out + 9, 8 + 32 + SC_MLKEM_CT);
        sc_sha256_final(&h, c->th);
    }
    sc_hkdf_extract(c->th, 32, ikm, 64, c->prk);
    expand(k_hs_r, c->prk, "hs r", 0);
    seal_identity(out + 49 + SC_MLKEM_CT, k_hs_r, "shusha-ch-R", c->th);
    {
        uint8_t th2[32];
        hash2(th2, c->th, 32, out + 49 + SC_MLKEM_CT, 112);
        mem_copy(c->th, th2, 32);
    }
    (void)net_send(my_port, from->ip, from->port, out, sizeof(out));
    mem_wipe(ss, sizeof(ss));
    mem_wipe(ikm, sizeof(ikm));
    mem_wipe(k_hs_r, sizeof(k_hs_r));
}

static void finish_keys(chan_t *c, int initiator) {
    uint8_t a[32], b[32];
    expand(a, c->prk, "data i2r", c->th);
    expand(b, c->prk, "data r2i", c->th);
    mem_copy(c->k_send, initiator ? a : b, 32);
    mem_copy(c->k_recv, initiator ? b : a, 32);
    c->send_ctr = 1;
    c->recv_max = 0;
    c->recv_bits = 0;
    c->state = ST_OPEN;
    mem_wipe(c->prk, 32);
    mem_wipe(c->e_priv, 32);
    mem_wipe(c->kem_dk, sizeof(c->kem_dk));
    mem_wipe(a, 32);
    mem_wipe(b, 32);
}

static void on_hs2(const uint8_t *p, long n, const net_addr_t *from) {
    uint8_t ss[32], ikm[64], k_hs_r[32], k_hs_i[32], out[1 + 8 + 112];
    chan_t *c;
    int i;
    if (n != 1 + 8 + 8 + 32 + SC_MLKEM_CT + 112) return;
    i = find_local_sid(p + 1);
    if (i < 0 || chans[i].state != ST_SENT_HS1) return;
    c = &chans[i];
    mem_copy(c->sid_peer, p + 9, 8);
    sc_mlkem768_decaps(ss, p + 49, c->kem_dk);
    sc_x25519(ikm, c->e_priv, p + 17);
    mem_copy(ikm + 32, ss, 32);
    {
        sc_sha256_t h;
        static const char lab[] = "shusha-ch-v1";
        sc_sha256_init(&h);
        sc_sha256_update(&h, (const uint8_t *)lab, sizeof(lab) - 1);
        sc_sha256_update(&h, c->hs1, sizeof(c->hs1));
        sc_sha256_update(&h, p + 9, 8 + 32 + SC_MLKEM_CT);
        sc_sha256_final(&h, c->th);
    }
    sc_hkdf_extract(c->th, 32, ikm, 64, c->prk);
    expand(k_hs_r, c->prk, "hs r", 0);
    expand(k_hs_i, c->prk, "hs i", 0);
    if (open_identity(c->peer_pub, p + 49 + SC_MLKEM_CT, k_hs_r, "shusha-ch-R", c->th) != 0) {
        chan_close(i);
        goto out;
    }
    {
        uint8_t th2[32];
        hash2(th2, c->th, 32, p + 49 + SC_MLKEM_CT, 112);
        mem_copy(c->th, th2, 32);
    }
    out[0] = HS3;
    mem_copy(out + 1, c->sid_peer, 8);
    seal_identity(out + 9, k_hs_i, "shusha-ch-I", c->th);
    {
        uint8_t th3[32];
        hash2(th3, c->th, 32, out + 9, 112);
        mem_copy(c->th, th3, 32);
    }
    (void)net_send(my_port, from->ip, from->port, out, sizeof(out));
    finish_keys(c, 1);
out:
    mem_wipe(ss, sizeof(ss));
    mem_wipe(ikm, sizeof(ikm));
    mem_wipe(k_hs_r, sizeof(k_hs_r));
    mem_wipe(k_hs_i, sizeof(k_hs_i));
}

static void on_hs3(const uint8_t *p, long n, const net_addr_t *from) {
    uint8_t k_hs_i[32];
    chan_t *c;
    int i;
    if (n != 1 + 8 + 112) return;
    i = find_local_sid(p + 1);
    if (i < 0 || chans[i].state != ST_SENT_HS2 || !mem_eq_ct(chans[i].peer.ip, from->ip, 4)) return;
    c = &chans[i];
    expand(k_hs_i, c->prk, "hs i", 0);
    if (open_identity(c->peer_pub, p + 9, k_hs_i, "shusha-ch-I", c->th) != 0) {
        chan_close(i);
        mem_wipe(k_hs_i, 32);
        return;
    }
    {
        uint8_t th3[32];
        hash2(th3, c->th, 32, p + 9, 112);
        mem_copy(c->th, th3, 32);
    }
    finish_keys(c, 0);
    mem_wipe(k_hs_i, 32);
}

/* Decrypt a DATA packet into out; 1 on success. */
static int on_data(const uint8_t *p, long n, const net_addr_t *from, chan_msg_t *out) {
    uint8_t nonce[12];
    uint64_t ctr;
    chan_t *c;
    int i;
    if (n < 1 + 8 + 8 + 16 || n - 33 > CHAN_MAX_MSG) return 0;
    i = find_local_sid(p + 1);
    if (i < 0 || chans[i].state != ST_OPEN) return 0;
    c = &chans[i];
    ctr = (uint64_t)sc_load32_le(p + 9) | ((uint64_t)sc_load32_le(p + 13) << 32);
    /* Replay window: reject duplicates and anything older than 64 packets. */
    if (ctr == 0 || (ctr <= c->recv_max && (c->recv_max - ctr >= 64 || (c->recv_bits >> (c->recv_max - ctr)) & 1))) {
        return 0;
    }
    nonce_for(nonce, ctr);
    if (sc_aead_open(out->data, p + 17, (uint64_t)(n - 33), p + n - 16, p, 17, nonce, c->k_recv) != 0) return 0;
    if (ctr > c->recv_max) {
        uint64_t shift = ctr - c->recv_max;
        c->recv_bits = shift >= 64 ? 1 : (c->recv_bits << shift) | 1;
        c->recv_max = ctr;
    } else {
        c->recv_bits |= 1ULL << (c->recv_max - ctr);
    }
    c->peer = *from; /* follow the peer if its address changed */
    out->chan = i;
    mem_copy(out->peer_pub, c->peer_pub, 32);
    out->from = *from;
    out->len = (uint16_t)(n - 33);
    return 1;
}

/* Process one datagram; returns 1 and fills out for application data. */
static int pump_one(chan_msg_t *out, int *got) {
    net_addr_t from;
    long n = net_recv(pkt, sizeof(pkt), &from, 0);
    *got = 0;
    if (n <= 0) return 0;
    switch (pkt[0]) {
    case HS1: on_hs1(pkt, n, &from); break;
    case HS2: on_hs2(pkt, n, &from); break;
    case HS3: on_hs3(pkt, n, &from); break;
    case DATA: *got = on_data(pkt, n, &from, out); break;
    }
    return 1;
}

int chan_poll(chan_msg_t *out) {
    int got;
    if (backlog_n) {
        *out = backlog[0];
        for (int i = 1; i < backlog_n; i++) backlog[i - 1] = backlog[i];
        backlog_n--;
        return 1;
    }
    while (pump_one(out, &got)) {
        if (got) return 1;
    }
    return 0;
}

int chan_connect(const uint8_t ip[4], uint16_t port, uint64_t timeout_ticks) {
    uint64_t end = sh_ticks() + timeout_ticks, resend = 0;
    uint8_t seed[64];
    chan_t *c;
    int i;

    for (i = 0; i < CHAN_MAX; i++) {
        if (chans[i].state == ST_OPEN && mem_eq_ct(chans[i].peer.ip, ip, 4) && chans[i].peer.port == port) return i;
    }
    i = alloc_chan();
    c = &chans[i];
    c->state = ST_SENT_HS1;
    mem_copy(c->peer.ip, ip, 4);
    c->peer.port = port;
    rand_bytes(c->sid_local, 8);
    rand_bytes(c->e_priv, 32);
    sc_x25519_base(c->e_pub, c->e_priv);
    rand_bytes(seed, 64);
    sc_mlkem768_keygen(c->kem_ek, c->kem_dk, seed);
    mem_wipe(seed, sizeof(seed));
    c->hs1[0] = HS1;
    mem_copy(c->hs1 + 1, c->sid_local, 8);
    mem_copy(c->hs1 + 9, c->e_pub, 32);
    mem_copy(c->hs1 + 41, c->kem_ek, SC_MLKEM_EK);

    while (sh_ticks() < end) {
        chan_msg_t m;
        int got;
        if (sh_ticks() >= resend) {
            (void)net_send(my_port, ip, port, c->hs1, sizeof(c->hs1));
            resend = sh_ticks() + 50;
        }
        while (pump_one(&m, &got)) {
            if (got && backlog_n < BACKLOG) backlog[backlog_n++] = m;
        }
        if (c->state == ST_OPEN) return i;
        if (c->state == ST_FREE) return -1; /* peer failed authentication */
        sh_sleep(1);
    }
    chan_close(i);
    return -1;
}

int chan_send(int i, const void *data, uint64_t len) {
    uint8_t nonce[12];
    chan_t *c;
    if (i < 0 || i >= CHAN_MAX || len > CHAN_MAX_MSG) return -1;
    c = &chans[i];
    if (c->state != ST_OPEN) return -1;
    pkt[0] = DATA;
    mem_copy(pkt + 1, c->sid_peer, 8);
    sc_store64_le(pkt + 9, c->send_ctr);
    nonce_for(nonce, c->send_ctr);
    c->send_ctr++;
    sc_aead_seal(pkt + 17, pkt + 17 + len, data, len, pkt, 17, nonce, c->k_send);
    return net_send(my_port, c->peer.ip, c->peer.port, pkt, 17 + len + 16);
}
