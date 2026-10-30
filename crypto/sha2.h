#ifndef SC_SHA2_H
#define SC_SHA2_H

#include <stdint.h>
#include <stddef.h>

/* SHA-256 and SHA-512 (FIPS 180-4), HMAC-SHA-256 (RFC 2104) and
 * HKDF-SHA-256 (RFC 5869). */

typedef struct {
    uint32_t h[8];
    uint64_t len;
    uint8_t  buf[64];
    size_t   used;
} sc_sha256_t;

typedef struct {
    uint64_t h[8];
    uint64_t len;
    uint8_t  buf[128];
    size_t   used;
} sc_sha512_t;

void sc_sha256_init(sc_sha256_t *st);
void sc_sha256_update(sc_sha256_t *st, const uint8_t *m, size_t len);
void sc_sha256_final(sc_sha256_t *st, uint8_t out[32]);
void sc_sha256(const uint8_t *m, size_t len, uint8_t out[32]);

void sc_sha512_init(sc_sha512_t *st);
void sc_sha512_update(sc_sha512_t *st, const uint8_t *m, size_t len);
void sc_sha512_final(sc_sha512_t *st, uint8_t out[64]);
void sc_sha512(const uint8_t *m, size_t len, uint8_t out[64]);

void sc_hmac_sha256(const uint8_t *key, size_t key_len,
                    const uint8_t *m, size_t len, uint8_t out[32]);

void sc_hkdf_extract(const uint8_t *salt, size_t salt_len,
                     const uint8_t *ikm, size_t ikm_len, uint8_t prk[32]);
/* Returns 0, or -1 if out_len > 255 * 32. */
int  sc_hkdf_expand(const uint8_t prk[32], const uint8_t *info, size_t info_len,
                    uint8_t *out, size_t out_len);
int  sc_hkdf(const uint8_t *salt, size_t salt_len,
             const uint8_t *ikm, size_t ikm_len,
             const uint8_t *info, size_t info_len,
             uint8_t *out, size_t out_len);

#endif
