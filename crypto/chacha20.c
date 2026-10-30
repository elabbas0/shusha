#include "chacha20.h"
#include "sc.h"

#define ROTL(v, n) (((v) << (n)) | ((v) >> (32 - (n))))
#define QR(a, b, c, d)                  \
    a += b; d ^= a; d = ROTL(d, 16);    \
    c += d; b ^= c; b = ROTL(b, 12);    \
    a += b; d ^= a; d = ROTL(d, 8);     \
    c += d; b ^= c; b = ROTL(b, 7)

static void chacha_init(uint32_t s[16], const uint8_t key[32]) {
    s[0] = 0x61707865;
    s[1] = 0x3320646e;
    s[2] = 0x79622d32;
    s[3] = 0x6b206574;
    for (int i = 0; i < 8; i++) s[4 + i] = sc_load32_le(key + 4 * i);
}

static void chacha_rounds(uint32_t x[16]) {
    for (int i = 0; i < 10; i++) {
        QR(x[0], x[4], x[8], x[12]);
        QR(x[1], x[5], x[9], x[13]);
        QR(x[2], x[6], x[10], x[14]);
        QR(x[3], x[7], x[11], x[15]);
        QR(x[0], x[5], x[10], x[15]);
        QR(x[1], x[6], x[11], x[12]);
        QR(x[2], x[7], x[8], x[13]);
        QR(x[3], x[4], x[9], x[14]);
    }
}

void sc_chacha20_block(const uint8_t key[32], uint32_t counter,
                       const uint8_t nonce[12], uint8_t out[64]) {
    uint32_t s[16], x[16];

    chacha_init(s, key);
    s[12] = counter;
    s[13] = sc_load32_le(nonce);
    s[14] = sc_load32_le(nonce + 4);
    s[15] = sc_load32_le(nonce + 8);
    for (int i = 0; i < 16; i++) x[i] = s[i];
    chacha_rounds(x);
    for (int i = 0; i < 16; i++) sc_store32_le(out + 4 * i, x[i] + s[i]);
    sc_wipe(s, sizeof(s));
    sc_wipe(x, sizeof(x));
}

void sc_chacha20_xor(const uint8_t key[32], const uint8_t nonce[12], uint32_t counter,
                     const uint8_t *in, uint8_t *out, size_t len) {
    uint8_t ks[64];

    while (len > 0) {
        size_t n = len < 64 ? len : 64;
        sc_chacha20_block(key, counter++, nonce, ks);
        for (size_t i = 0; i < n; i++) out[i] = in[i] ^ ks[i];
        in += n;
        out += n;
        len -= n;
    }
    sc_wipe(ks, sizeof(ks));
}

void sc_hchacha20(const uint8_t key[32], const uint8_t nonce[16], uint8_t out[32]) {
    uint32_t x[16];

    chacha_init(x, key);
    for (int i = 0; i < 4; i++) x[12 + i] = sc_load32_le(nonce + 4 * i);
    chacha_rounds(x);
    for (int i = 0; i < 4; i++) {
        sc_store32_le(out + 4 * i, x[i]);
        sc_store32_le(out + 16 + 4 * i, x[12 + i]);
    }
    sc_wipe(x, sizeof(x));
}
