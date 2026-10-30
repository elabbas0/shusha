#ifndef SC_H
#define SC_H

/* Shusha crypto library: common helpers.
 *
 * Every file under crypto/ builds three ways: into the kernel, into
 * userspace programs, and on the host for the test-vector suite
 * (tests/crypto_test.c).  No libc, no allocation, no global state
 * except the DRBG.  All secret-dependent code is constant time. */

#include <stdint.h>
#include <stddef.h>

static inline uint32_t sc_load32_le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline void sc_store32_le(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static inline uint32_t sc_load32_be(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static inline void sc_store32_be(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static inline uint64_t sc_load64_be(const uint8_t *p) {
    return ((uint64_t)sc_load32_be(p) << 32) | sc_load32_be(p + 4);
}

static inline void sc_store64_be(uint8_t *p, uint64_t v) {
    sc_store32_be(p, (uint32_t)(v >> 32));
    sc_store32_be(p + 4, (uint32_t)v);
}

static inline void sc_store64_le(uint8_t *p, uint64_t v) {
    sc_store32_le(p, (uint32_t)v);
    sc_store32_le(p + 4, (uint32_t)(v >> 32));
}

/* Overwrite secrets; volatile so the compiler cannot drop the stores. */
static inline void sc_wipe(void *p, size_t n) {
    volatile uint8_t *d = (volatile uint8_t *)p;
    while (n--) *d++ = 0;
}

/* Constant-time equality: returns 1 if equal, 0 otherwise. */
static inline int sc_equal(const uint8_t *a, const uint8_t *b, size_t n) {
    uint32_t d = 0;
    for (size_t i = 0; i < n; i++) d |= (uint32_t)(a[i] ^ b[i]);
    return (int)(1 & ((d - 1) >> 8));
}

#endif
