#ifndef SC_POLY1305_H
#define SC_POLY1305_H

#include <stdint.h>
#include <stddef.h>

/* Poly1305 one-time authenticator (RFC 8439), 26-bit limbs. */
typedef struct {
    uint32_t r[5];
    uint32_t h[5];
    uint32_t pad[4];
    uint8_t  buf[16];
    size_t   used;
} sc_poly1305_t;

void sc_poly1305_init(sc_poly1305_t *st, const uint8_t key[32]);
void sc_poly1305_update(sc_poly1305_t *st, const uint8_t *m, size_t len);
void sc_poly1305_final(sc_poly1305_t *st, uint8_t tag[16]);

void sc_poly1305(const uint8_t key[32], const uint8_t *m, size_t len, uint8_t tag[16]);

#endif
