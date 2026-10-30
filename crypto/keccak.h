#ifndef SC_KECCAK_H
#define SC_KECCAK_H

#include <stdint.h>
#include <stddef.h>

/* SHA-3 and SHAKE (FIPS 202).  The incremental interface is used for
 * SHAKE as an XOF: absorb, finalize once, then squeeze any amount. */

typedef struct {
    uint64_t s[25];
    size_t rate;   /* bytes */
    size_t pos;
    uint8_t pad;   /* 0x06 SHA-3, 0x1f SHAKE */
} sc_keccak_t;

void sc_keccak_init(sc_keccak_t *k, size_t rate, uint8_t pad);
void sc_keccak_absorb(sc_keccak_t *k, const uint8_t *in, size_t len);
void sc_keccak_finalize(sc_keccak_t *k);
void sc_keccak_squeeze(sc_keccak_t *k, uint8_t *out, size_t len);

#define SC_SHAKE128_RATE 168
#define SC_SHAKE256_RATE 136

void sc_sha3_256(const uint8_t *in, size_t len, uint8_t out[32]);
void sc_sha3_512(const uint8_t *in, size_t len, uint8_t out[64]);
void sc_shake128(const uint8_t *in, size_t len, uint8_t *out, size_t outlen);
void sc_shake256(const uint8_t *in, size_t len, uint8_t *out, size_t outlen);

#endif
