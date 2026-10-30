#ifndef SC_BLAKE2B_H
#define SC_BLAKE2B_H

#include <stdint.h>
#include <stddef.h>

/* BLAKE2b (RFC 7693), 1..64-byte output, optional key. */

typedef struct {
    uint64_t h[8];
    uint64_t t[2];
    uint8_t  buf[128];
    size_t   used;
    size_t   outlen;
} sc_blake2b_t;

void sc_blake2b_init(sc_blake2b_t *st, size_t outlen, const uint8_t *key, size_t keylen);
void sc_blake2b_update(sc_blake2b_t *st, const uint8_t *in, size_t len);
void sc_blake2b_final(sc_blake2b_t *st, uint8_t *out);
void sc_blake2b(uint8_t *out, size_t outlen, const uint8_t *key, size_t keylen,
                const uint8_t *in, size_t len);

#endif
