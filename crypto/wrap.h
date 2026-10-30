#ifndef SC_WRAP_H
#define SC_WRAP_H

#include <stdint.h>
#include <stddef.h>
#include "mlkem.h"

/* Hybrid key wrap (docs/CRYPTO.md section 2): a 32-byte key is sealed to
 * a recipient's X25519 + ML-KEM-768 public keys.  Recovering it requires
 * breaking both.  The `purpose` string binds the box to its context
 * (e.g. "fk:<file id>:<epoch>") and must match on unwrap.
 *
 * box = e_pub(32) || mlkem_ct(1088) || nonce(24) || ct(32) || tag(16) */

#define SC_WRAP_BOX (32 + SC_MLKEM_CT + 24 + 32 + 16)
#define SC_WRAP_RANDOM 88   /* X25519 ephemeral(32) || ML-KEM m(32) || nonce(24) */
#define SC_WRAP_PURPOSE_MAX 128

typedef struct {
    uint8_t x25519[32];
    uint8_t mlkem[SC_MLKEM_EK];
} sc_wrap_pub_t;

typedef struct {
    uint8_t x25519[32];
    uint8_t mlkem[SC_MLKEM_DK];
} sc_wrap_priv_t;

/* Key pair from 96 random bytes (X25519 secret 32 || ML-KEM d||z 64). */
void sc_wrap_keypair(sc_wrap_pub_t *pub, sc_wrap_priv_t *priv, const uint8_t seed[96]);
/* Recipient key id: first 16 bytes of SHA-256(x25519 || mlkem). */
void sc_wrap_keyid(uint8_t id[16], const sc_wrap_pub_t *pub);

int sc_wrap(uint8_t box[SC_WRAP_BOX], const sc_wrap_pub_t *to, const uint8_t key[32],
            const uint8_t *purpose, size_t purpose_len, const uint8_t rnd[SC_WRAP_RANDOM]);
/* Returns 0 and the key, or -1 (tampered box, wrong recipient or purpose). */
int sc_unwrap(uint8_t key[32], const uint8_t box[SC_WRAP_BOX], const sc_wrap_pub_t *me,
              const sc_wrap_priv_t *priv, const uint8_t *purpose, size_t purpose_len);

#endif
