#ifndef KERNEL_RANDOM_H
#define KERNEL_RANDOM_H

#include <stdint.h>

/*
 * Kernel CSPRNG.  Seeded once at boot from RDSEED/RDRAND (when the CPU
 * has them) and CPU timing jitter, then served by the ChaCha20 DRBG in
 * crypto/drbg.c.  Every request mixes in fresh timer (and hardware, if
 * present) input, so the state keeps moving even without new events.
 *
 * All key material in the kernel must come from random_get().
 */

#define RANDOM_SRC_RDSEED 1u
#define RANDOM_SRC_RDRAND 2u
#define RANDOM_SRC_JITTER 4u

/* Collect boot entropy and seed.  Returns the RANDOM_SRC_* mask used. */
uint32_t random_init(void);

int random_ready(void);

/* Fill buf with n random bytes.  Returns 0, or -1 before random_init(). */
int random_get(void *buf, uint64_t n);

/* Mix an event (interrupt time, packet arrival, ...) into the pool. */
void random_add_event(uint64_t value);

#endif
