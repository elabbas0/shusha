#include "poly1305.h"
#include "sc.h"

void sc_poly1305_init(sc_poly1305_t *st, const uint8_t key[32]) {
    /* r is clamped as the RFC requires. */
    st->r[0] = (sc_load32_le(key + 0)) & 0x3ffffff;
    st->r[1] = (sc_load32_le(key + 3) >> 2) & 0x3ffff03;
    st->r[2] = (sc_load32_le(key + 6) >> 4) & 0x3ffc0ff;
    st->r[3] = (sc_load32_le(key + 9) >> 6) & 0x3f03fff;
    st->r[4] = (sc_load32_le(key + 12) >> 8) & 0x00fffff;
    for (int i = 0; i < 5; i++) st->h[i] = 0;
    for (int i = 0; i < 4; i++) st->pad[i] = sc_load32_le(key + 16 + 4 * i);
    st->used = 0;
}

/* Absorb one 16-byte block; hibit is 1<<24 for full blocks, 0 for the
 * already-padded final partial block. */
static void poly_block(sc_poly1305_t *st, const uint8_t m[16], uint32_t hibit) {
    const uint32_t r0 = st->r[0], r1 = st->r[1], r2 = st->r[2], r3 = st->r[3], r4 = st->r[4];
    const uint32_t s1 = r1 * 5, s2 = r2 * 5, s3 = r3 * 5, s4 = r4 * 5;
    uint32_t h0 = st->h[0], h1 = st->h[1], h2 = st->h[2], h3 = st->h[3], h4 = st->h[4];
    uint64_t d0, d1, d2, d3, d4;
    uint32_t c;

    h0 += (sc_load32_le(m + 0)) & 0x3ffffff;
    h1 += (sc_load32_le(m + 3) >> 2) & 0x3ffffff;
    h2 += (sc_load32_le(m + 6) >> 4) & 0x3ffffff;
    h3 += (sc_load32_le(m + 9) >> 6) & 0x3ffffff;
    h4 += (sc_load32_le(m + 12) >> 8) | hibit;

    d0 = (uint64_t)h0 * r0 + (uint64_t)h1 * s4 + (uint64_t)h2 * s3 + (uint64_t)h3 * s2 + (uint64_t)h4 * s1;
    d1 = (uint64_t)h0 * r1 + (uint64_t)h1 * r0 + (uint64_t)h2 * s4 + (uint64_t)h3 * s3 + (uint64_t)h4 * s2;
    d2 = (uint64_t)h0 * r2 + (uint64_t)h1 * r1 + (uint64_t)h2 * r0 + (uint64_t)h3 * s4 + (uint64_t)h4 * s3;
    d3 = (uint64_t)h0 * r3 + (uint64_t)h1 * r2 + (uint64_t)h2 * r1 + (uint64_t)h3 * r0 + (uint64_t)h4 * s4;
    d4 = (uint64_t)h0 * r4 + (uint64_t)h1 * r3 + (uint64_t)h2 * r2 + (uint64_t)h3 * r1 + (uint64_t)h4 * r0;

    c = (uint32_t)(d0 >> 26); h0 = (uint32_t)d0 & 0x3ffffff;
    d1 += c; c = (uint32_t)(d1 >> 26); h1 = (uint32_t)d1 & 0x3ffffff;
    d2 += c; c = (uint32_t)(d2 >> 26); h2 = (uint32_t)d2 & 0x3ffffff;
    d3 += c; c = (uint32_t)(d3 >> 26); h3 = (uint32_t)d3 & 0x3ffffff;
    d4 += c; c = (uint32_t)(d4 >> 26); h4 = (uint32_t)d4 & 0x3ffffff;
    h0 += c * 5; c = h0 >> 26; h0 &= 0x3ffffff;
    h1 += c;

    st->h[0] = h0; st->h[1] = h1; st->h[2] = h2; st->h[3] = h3; st->h[4] = h4;
}

void sc_poly1305_update(sc_poly1305_t *st, const uint8_t *m, size_t len) {
    if (st->used) {
        while (len && st->used < 16) {
            st->buf[st->used++] = *m++;
            len--;
        }
        if (st->used < 16) return;
        poly_block(st, st->buf, 1u << 24);
        st->used = 0;
    }
    while (len >= 16) {
        poly_block(st, m, 1u << 24);
        m += 16;
        len -= 16;
    }
    while (len--) st->buf[st->used++] = *m++;
}

void sc_poly1305_final(sc_poly1305_t *st, uint8_t tag[16]) {
    uint32_t h0, h1, h2, h3, h4, c;
    uint32_t g0, g1, g2, g3, g4, mask;
    uint64_t f;

    if (st->used) {
        st->buf[st->used++] = 1;
        while (st->used < 16) st->buf[st->used++] = 0;
        poly_block(st, st->buf, 0);
    }

    h0 = st->h[0]; h1 = st->h[1]; h2 = st->h[2]; h3 = st->h[3]; h4 = st->h[4];

    c = h1 >> 26; h1 &= 0x3ffffff;
    h2 += c; c = h2 >> 26; h2 &= 0x3ffffff;
    h3 += c; c = h3 >> 26; h3 &= 0x3ffffff;
    h4 += c; c = h4 >> 26; h4 &= 0x3ffffff;
    h0 += c * 5; c = h0 >> 26; h0 &= 0x3ffffff;
    h1 += c;

    /* g = h - p; select h or g in constant time. */
    g0 = h0 + 5; c = g0 >> 26; g0 &= 0x3ffffff;
    g1 = h1 + c; c = g1 >> 26; g1 &= 0x3ffffff;
    g2 = h2 + c; c = g2 >> 26; g2 &= 0x3ffffff;
    g3 = h3 + c; c = g3 >> 26; g3 &= 0x3ffffff;
    g4 = h4 + c - (1u << 26);

    mask = (g4 >> 31) - 1;
    g0 &= mask; g1 &= mask; g2 &= mask; g3 &= mask; g4 &= mask;
    mask = ~mask;
    h0 = (h0 & mask) | g0;
    h1 = (h1 & mask) | g1;
    h2 = (h2 & mask) | g2;
    h3 = (h3 & mask) | g3;
    h4 = (h4 & mask) | g4;

    h0 = (h0) | (h1 << 26);
    h1 = (h1 >> 6) | (h2 << 20);
    h2 = (h2 >> 12) | (h3 << 14);
    h3 = (h3 >> 18) | (h4 << 8);

    f = (uint64_t)h0 + st->pad[0]; h0 = (uint32_t)f;
    f = (uint64_t)h1 + st->pad[1] + (f >> 32); h1 = (uint32_t)f;
    f = (uint64_t)h2 + st->pad[2] + (f >> 32); h2 = (uint32_t)f;
    f = (uint64_t)h3 + st->pad[3] + (f >> 32); h3 = (uint32_t)f;

    sc_store32_le(tag + 0, h0);
    sc_store32_le(tag + 4, h1);
    sc_store32_le(tag + 8, h2);
    sc_store32_le(tag + 12, h3);
    sc_wipe(st, sizeof(*st));
}

void sc_poly1305(const uint8_t key[32], const uint8_t *m, size_t len, uint8_t tag[16]) {
    sc_poly1305_t st;
    sc_poly1305_init(&st, key);
    sc_poly1305_update(&st, m, len);
    sc_poly1305_final(&st, tag);
}
