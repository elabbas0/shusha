#include "aead.h"
#include "chacha20.h"
#include "poly1305.h"
#include "sc.h"

static const uint8_t zeros[16];

/* Tag over aad || pad16 || ct || pad16 || le64(aad_len) || le64(len). */
static void aead_tag(uint8_t tag[16], const uint8_t *ct, size_t len,
                     const uint8_t *aad, size_t aad_len,
                     const uint8_t nonce[12], const uint8_t key[32]) {
    uint8_t block[64];
    uint8_t lens[16];
    sc_poly1305_t st;

    sc_chacha20_block(key, 0, nonce, block);
    sc_poly1305_init(&st, block);
    sc_poly1305_update(&st, aad, aad_len);
    sc_poly1305_update(&st, zeros, (16 - aad_len % 16) % 16);
    sc_poly1305_update(&st, ct, len);
    sc_poly1305_update(&st, zeros, (16 - len % 16) % 16);
    sc_store64_le(lens, (uint64_t)aad_len);
    sc_store64_le(lens + 8, (uint64_t)len);
    sc_poly1305_update(&st, lens, 16);
    sc_poly1305_final(&st, tag);
    sc_wipe(block, sizeof(block));
}

void sc_aead_seal(uint8_t *ct, uint8_t tag[16],
                  const uint8_t *pt, size_t len,
                  const uint8_t *aad, size_t aad_len,
                  const uint8_t nonce[12], const uint8_t key[32]) {
    sc_chacha20_xor(key, nonce, 1, pt, ct, len);
    aead_tag(tag, ct, len, aad, aad_len, nonce, key);
}

int sc_aead_open(uint8_t *pt,
                 const uint8_t *ct, size_t len, const uint8_t tag[16],
                 const uint8_t *aad, size_t aad_len,
                 const uint8_t nonce[12], const uint8_t key[32]) {
    uint8_t expect[16];
    int ok;

    aead_tag(expect, ct, len, aad, aad_len, nonce, key);
    ok = sc_equal(expect, tag, 16);
    sc_wipe(expect, sizeof(expect));
    if (!ok) return -1;
    sc_chacha20_xor(key, nonce, 1, ct, pt, len);
    return 0;
}

/* XChaCha: subkey = HChaCha20(key, nonce[0..16]), then ChaCha20 with
 * nonce = 0x00000000 || nonce[16..24]. */
static void xsetup(uint8_t subkey[32], uint8_t n12[12], const uint8_t nonce[24], const uint8_t key[32]) {
    sc_hchacha20(key, nonce, subkey);
    for (int i = 0; i < 4; i++) n12[i] = 0;
    for (int i = 0; i < 8; i++) n12[4 + i] = nonce[16 + i];
}

void sc_xaead_seal(uint8_t *ct, uint8_t tag[16],
                   const uint8_t *pt, size_t len,
                   const uint8_t *aad, size_t aad_len,
                   const uint8_t nonce[24], const uint8_t key[32]) {
    uint8_t subkey[32], n12[12];
    xsetup(subkey, n12, nonce, key);
    sc_aead_seal(ct, tag, pt, len, aad, aad_len, n12, subkey);
    sc_wipe(subkey, sizeof(subkey));
}

int sc_xaead_open(uint8_t *pt,
                  const uint8_t *ct, size_t len, const uint8_t tag[16],
                  const uint8_t *aad, size_t aad_len,
                  const uint8_t nonce[24], const uint8_t key[32]) {
    uint8_t subkey[32], n12[12];
    int r;
    xsetup(subkey, n12, nonce, key);
    r = sc_aead_open(pt, ct, len, tag, aad, aad_len, n12, subkey);
    sc_wipe(subkey, sizeof(subkey));
    return r;
}
