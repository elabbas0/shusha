#ifndef SC_AEAD_H
#define SC_AEAD_H

#include <stdint.h>
#include <stddef.h>

/* ChaCha20-Poly1305 (RFC 8439) and XChaCha20-Poly1305
 * (draft-irtf-cfrg-xchacha).  ct and pt may alias.  Tags are 16 bytes,
 * appended by seal and checked by open.  open returns 0 on success and
 * -1 on a bad tag, in which case it writes no plaintext at all. */

#define SC_AEAD_KEY   32
#define SC_AEAD_TAG   16
#define SC_AEAD_NONCE 12
#define SC_XAEAD_NONCE 24

void sc_aead_seal(uint8_t *ct, uint8_t tag[16],
                  const uint8_t *pt, size_t len,
                  const uint8_t *aad, size_t aad_len,
                  const uint8_t nonce[12], const uint8_t key[32]);

int  sc_aead_open(uint8_t *pt,
                  const uint8_t *ct, size_t len, const uint8_t tag[16],
                  const uint8_t *aad, size_t aad_len,
                  const uint8_t nonce[12], const uint8_t key[32]);

void sc_xaead_seal(uint8_t *ct, uint8_t tag[16],
                   const uint8_t *pt, size_t len,
                   const uint8_t *aad, size_t aad_len,
                   const uint8_t nonce[24], const uint8_t key[32]);

int  sc_xaead_open(uint8_t *pt,
                   const uint8_t *ct, size_t len, const uint8_t tag[16],
                   const uint8_t *aad, size_t aad_len,
                   const uint8_t nonce[24], const uint8_t key[32]);

#endif
