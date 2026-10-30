#include "blake2b.h"
#include "sc.h"

static const uint64_t IV[8] = {
    0x6a09e667f3bcc908ULL, 0xbb67ae8584caa73bULL, 0x3c6ef372fe94f82bULL, 0xa54ff53a5f1d36f1ULL,
    0x510e527fade682d1ULL, 0x9b05688c2b3e6c1fULL, 0x1f83d9abfb41bd6bULL, 0x5be0cd19137e2179ULL
};

static const uint8_t SIGMA[12][16] = {
    { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 },
    { 14, 10, 4, 8, 9, 15, 13, 6, 1, 12, 0, 2, 11, 7, 5, 3 },
    { 11, 8, 12, 0, 5, 2, 15, 13, 10, 14, 3, 6, 7, 1, 9, 4 },
    { 7, 9, 3, 1, 13, 12, 11, 14, 2, 6, 5, 10, 4, 0, 15, 8 },
    { 9, 0, 5, 7, 2, 4, 10, 15, 14, 1, 11, 12, 6, 8, 3, 13 },
    { 2, 12, 6, 10, 0, 11, 8, 3, 4, 13, 7, 5, 15, 14, 1, 9 },
    { 12, 5, 1, 15, 14, 13, 4, 10, 0, 7, 6, 3, 9, 2, 8, 11 },
    { 13, 11, 7, 14, 12, 1, 3, 9, 5, 0, 15, 4, 8, 6, 2, 10 },
    { 6, 15, 14, 9, 11, 3, 0, 8, 12, 2, 13, 7, 1, 4, 10, 5 },
    { 10, 2, 8, 4, 7, 6, 1, 5, 15, 11, 9, 14, 3, 12, 13, 0 },
    { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 },
    { 14, 10, 4, 8, 9, 15, 13, 6, 1, 12, 0, 2, 11, 7, 5, 3 }
};

#define ROR64(v, n) (((v) >> (n)) | ((v) << (64 - (n))))
#define G(a, b, c, d, x, y)              \
    a = a + b + x; d = ROR64(d ^ a, 32); \
    c = c + d;     b = ROR64(b ^ c, 24); \
    a = a + b + y; d = ROR64(d ^ a, 16); \
    c = c + d;     b = ROR64(b ^ c, 63)

static uint64_t load64(const uint8_t *p) {
    return (uint64_t)sc_load32_le(p) | ((uint64_t)sc_load32_le(p + 4) << 32);
}

static void compress(sc_blake2b_t *st, const uint8_t block[128], int last) {
    uint64_t m[16], v[16];
    for (int i = 0; i < 16; i++) m[i] = load64(block + 8 * i);
    for (int i = 0; i < 8; i++) {
        v[i] = st->h[i];
        v[i + 8] = IV[i];
    }
    v[12] ^= st->t[0];
    v[13] ^= st->t[1];
    if (last) v[14] = ~v[14];
    for (int r = 0; r < 12; r++) {
        const uint8_t *s = SIGMA[r];
        G(v[0], v[4], v[8], v[12], m[s[0]], m[s[1]]);
        G(v[1], v[5], v[9], v[13], m[s[2]], m[s[3]]);
        G(v[2], v[6], v[10], v[14], m[s[4]], m[s[5]]);
        G(v[3], v[7], v[11], v[15], m[s[6]], m[s[7]]);
        G(v[0], v[5], v[10], v[15], m[s[8]], m[s[9]]);
        G(v[1], v[6], v[11], v[12], m[s[10]], m[s[11]]);
        G(v[2], v[7], v[8], v[13], m[s[12]], m[s[13]]);
        G(v[3], v[4], v[9], v[14], m[s[14]], m[s[15]]);
    }
    for (int i = 0; i < 8; i++) st->h[i] ^= v[i] ^ v[i + 8];
    sc_wipe(v, sizeof(v));
    sc_wipe(m, sizeof(m));
}

void sc_blake2b_init(sc_blake2b_t *st, size_t outlen, const uint8_t *key, size_t keylen) {
    for (int i = 0; i < 8; i++) st->h[i] = IV[i];
    st->h[0] ^= 0x01010000ULL ^ ((uint64_t)keylen << 8) ^ (uint64_t)outlen;
    st->t[0] = st->t[1] = 0;
    st->used = 0;
    st->outlen = outlen;
    if (keylen) {
        uint8_t block[128];
        for (int i = 0; i < 128; i++) block[i] = i < (int)keylen ? key[i] : 0;
        sc_blake2b_update(st, block, 128);
        sc_wipe(block, sizeof(block));
    }
}

void sc_blake2b_update(sc_blake2b_t *st, const uint8_t *in, size_t len) {
    while (len) {
        if (st->used == 128) {
            /* Only compress when more input follows: the last block must
             * be compressed by final() with the finalisation flag. */
            st->t[0] += 128;
            if (st->t[0] < 128) st->t[1]++;
            compress(st, st->buf, 0);
            st->used = 0;
        }
        st->buf[st->used++] = *in++;
        len--;
    }
}

void sc_blake2b_final(sc_blake2b_t *st, uint8_t *out) {
    uint8_t full[64];
    st->t[0] += st->used;
    if (st->t[0] < st->used) st->t[1]++;
    while (st->used < 128) st->buf[st->used++] = 0;
    compress(st, st->buf, 1);
    for (int i = 0; i < 8; i++) {
        sc_store32_le(full + 8 * i, (uint32_t)st->h[i]);
        sc_store32_le(full + 8 * i + 4, (uint32_t)(st->h[i] >> 32));
    }
    for (size_t i = 0; i < st->outlen; i++) out[i] = full[i];
    sc_wipe(full, sizeof(full));
    sc_wipe(st, sizeof(*st));
}

void sc_blake2b(uint8_t *out, size_t outlen, const uint8_t *key, size_t keylen,
                const uint8_t *in, size_t len) {
    sc_blake2b_t st;
    sc_blake2b_init(&st, outlen, key, keylen);
    sc_blake2b_update(&st, in, len);
    sc_blake2b_final(&st, out);
}
