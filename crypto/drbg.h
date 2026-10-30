#ifndef SC_DRBG_H
#define SC_DRBG_H

#include <stdint.h>
#include <stddef.h>

/* ChaCha20 deterministic random bit generator with fast key erasure:
 * every 64-byte block replaces the key with its first 32 bytes and
 * hands out the other 32, so a later state compromise cannot recover
 * earlier output.  Seeding/reseeding hashes new entropy into the key. */

typedef struct {
    uint8_t key[32];
    int     seeded;
} sc_drbg_t;

void sc_drbg_reseed(sc_drbg_t *st, const uint8_t *seed, size_t len);

/* Returns 0, or -1 if the generator was never seeded (no output). */
int  sc_drbg_generate(sc_drbg_t *st, uint8_t *out, size_t len);

#endif
