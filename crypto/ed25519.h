#ifndef SC_ED25519_H
#define SC_ED25519_H

#include <stdint.h>
#include <stddef.h>

/* Ed25519 signatures (RFC 8032, pure variant).
 * Keys are a 32-byte seed (secret) and a 32-byte public key. */

void sc_ed25519_public_key(uint8_t pk[32], const uint8_t seed[32]);

void sc_ed25519_sign(uint8_t sig[64], const uint8_t *m, size_t len,
                     const uint8_t seed[32], const uint8_t pk[32]);

/* Returns 0 if the signature is valid, -1 otherwise. */
int  sc_ed25519_verify(const uint8_t sig[64], const uint8_t *m, size_t len,
                       const uint8_t pk[32]);

#endif
