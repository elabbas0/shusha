#include "drbg.h"
#include "chacha20.h"
#include "sha2.h"
#include "sc.h"

void sc_drbg_reseed(sc_drbg_t *st, const uint8_t *seed, size_t len) {
    static const uint8_t label[] = "shusha-drbg-v1";
    sc_sha256_t h;

    sc_sha256_init(&h);
    sc_sha256_update(&h, label, sizeof(label) - 1);
    sc_sha256_update(&h, st->key, 32);
    sc_sha256_update(&h, seed, len);
    sc_sha256_final(&h, st->key);
    st->seeded = 1;
}

int sc_drbg_generate(sc_drbg_t *st, uint8_t *out, size_t len) {
    static const uint8_t nonce[12];
    uint8_t block[64];

    if (!st->seeded) return -1;
    while (len > 0) {
        size_t n = len < 32 ? len : 32;
        sc_chacha20_block(st->key, 0, nonce, block);
        for (int i = 0; i < 32; i++) st->key[i] = block[i];
        for (size_t i = 0; i < n; i++) out[i] = block[32 + i];
        out += n;
        len -= n;
    }
    sc_wipe(block, sizeof(block));
    return 0;
}
