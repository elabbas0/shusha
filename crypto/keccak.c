#include "keccak.h"
#include "sc.h"

static const uint64_t RC[24] = {
    0x0000000000000001ULL, 0x0000000000008082ULL, 0x800000000000808aULL, 0x8000000080008000ULL,
    0x000000000000808bULL, 0x0000000080000001ULL, 0x8000000080008081ULL, 0x8000000000008009ULL,
    0x000000000000008aULL, 0x0000000000000088ULL, 0x0000000080008009ULL, 0x000000008000000aULL,
    0x000000008000808bULL, 0x800000000000008bULL, 0x8000000000008089ULL, 0x8000000000008003ULL,
    0x8000000000008002ULL, 0x8000000000000080ULL, 0x000000000000800aULL, 0x800000008000000aULL,
    0x8000000080008081ULL, 0x8000000000008080ULL, 0x0000000080000001ULL, 0x8000000080008008ULL
};
static const uint8_t RHO[24] = { 1, 3, 6, 10, 15, 21, 28, 36, 45, 55, 2, 14, 27, 41, 56, 8, 25, 43, 62, 18, 39, 61, 20, 44 };
static const uint8_t PI[24] = { 10, 7, 11, 17, 18, 3, 5, 16, 8, 21, 24, 4, 15, 23, 19, 13, 12, 2, 20, 14, 22, 9, 6, 1 };

#define ROL64(v, n) (((v) << (n)) | ((v) >> (64 - (n))))

static void keccak_f(uint64_t a[25]) {
    uint64_t c[5], d, t, tmp;
    for (int r = 0; r < 24; r++) {
        for (int x = 0; x < 5; x++) c[x] = a[x] ^ a[x + 5] ^ a[x + 10] ^ a[x + 15] ^ a[x + 20];
        for (int x = 0; x < 5; x++) {
            d = c[(x + 4) % 5] ^ ROL64(c[(x + 1) % 5], 1);
            for (int y = 0; y < 25; y += 5) a[y + x] ^= d;
        }
        t = a[1];
        for (int i = 0; i < 24; i++) {
            tmp = a[PI[i]];
            a[PI[i]] = ROL64(t, RHO[i]);
            t = tmp;
        }
        for (int y = 0; y < 25; y += 5) {
            for (int x = 0; x < 5; x++) c[x] = a[y + x];
            for (int x = 0; x < 5; x++) a[y + x] = c[x] ^ (~c[(x + 1) % 5] & c[(x + 2) % 5]);
        }
        a[0] ^= RC[r];
    }
}

static void xor_byte(uint64_t s[25], size_t i, uint8_t v) {
    s[i / 8] ^= (uint64_t)v << (8 * (i % 8));
}

void sc_keccak_init(sc_keccak_t *k, size_t rate, uint8_t pad) {
    for (int i = 0; i < 25; i++) k->s[i] = 0;
    k->rate = rate;
    k->pos = 0;
    k->pad = pad;
}

void sc_keccak_absorb(sc_keccak_t *k, const uint8_t *in, size_t len) {
    while (len--) {
        xor_byte(k->s, k->pos++, *in++);
        if (k->pos == k->rate) {
            keccak_f(k->s);
            k->pos = 0;
        }
    }
}

void sc_keccak_finalize(sc_keccak_t *k) {
    xor_byte(k->s, k->pos, k->pad);
    xor_byte(k->s, k->rate - 1, 0x80);
    keccak_f(k->s);
    k->pos = 0;
}

void sc_keccak_squeeze(sc_keccak_t *k, uint8_t *out, size_t len) {
    while (len--) {
        if (k->pos == k->rate) {
            keccak_f(k->s);
            k->pos = 0;
        }
        *out++ = (uint8_t)(k->s[k->pos / 8] >> (8 * (k->pos % 8)));
        k->pos++;
    }
}

static void oneshot(size_t rate, uint8_t pad, const uint8_t *in, size_t len, uint8_t *out, size_t outlen) {
    sc_keccak_t k;
    sc_keccak_init(&k, rate, pad);
    sc_keccak_absorb(&k, in, len);
    sc_keccak_finalize(&k);
    sc_keccak_squeeze(&k, out, outlen);
    sc_wipe(&k, sizeof(k));
}

void sc_sha3_256(const uint8_t *in, size_t len, uint8_t out[32]) { oneshot(136, 0x06, in, len, out, 32); }
void sc_sha3_512(const uint8_t *in, size_t len, uint8_t out[64]) { oneshot(72, 0x06, in, len, out, 64); }
void sc_shake128(const uint8_t *in, size_t len, uint8_t *out, size_t outlen) { oneshot(168, 0x1f, in, len, out, outlen); }
void sc_shake256(const uint8_t *in, size_t len, uint8_t *out, size_t outlen) { oneshot(136, 0x1f, in, len, out, outlen); }
