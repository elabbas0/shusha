#ifndef SC_CHACHA20_H
#define SC_CHACHA20_H

#include <stdint.h>
#include <stddef.h>

/* ChaCha20 (RFC 8439): 256-bit key, 96-bit nonce, 32-bit block counter. */
void sc_chacha20_block(const uint8_t key[32], uint32_t counter,
                       const uint8_t nonce[12], uint8_t out[64]);

/* XOR `len` bytes of keystream (starting at block `counter`) into out. */
void sc_chacha20_xor(const uint8_t key[32], const uint8_t nonce[12], uint32_t counter,
                     const uint8_t *in, uint8_t *out, size_t len);

/* HChaCha20 (draft-irtf-cfrg-xchacha): derive a subkey from a 128-bit nonce. */
void sc_hchacha20(const uint8_t key[32], const uint8_t nonce[16], uint8_t out[32]);

#endif
