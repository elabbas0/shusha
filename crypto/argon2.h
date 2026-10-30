#ifndef SC_ARGON2_H
#define SC_ARGON2_H

#include <stdint.h>
#include <stddef.h>

/* Argon2id, version 0x13 (RFC 9106).  The caller provides the working
 * memory: `mem_kib` KiB, i.e. sc_argon2_mem_bytes(mem_kib, lanes) bytes,
 * 8-byte aligned.  Lanes are processed one after another (no threads).
 * Returns 0, or -1 on invalid parameters. */

#define SC_ARGON2_BLOCK 1024

static inline size_t sc_argon2_mem_bytes(uint32_t mem_kib, uint32_t lanes) {
    uint32_t m = mem_kib < 8 * lanes ? 8 * lanes : mem_kib;
    return (size_t)(m / (4 * lanes) * 4 * lanes) * SC_ARGON2_BLOCK;
}

int sc_argon2id(uint8_t *out, uint32_t outlen,
                const uint8_t *pwd, uint32_t pwdlen,
                const uint8_t *salt, uint32_t saltlen,
                const uint8_t *secret, uint32_t secretlen,
                const uint8_t *ad, uint32_t adlen,
                uint32_t passes, uint32_t mem_kib, uint32_t lanes,
                void *work);

#endif
