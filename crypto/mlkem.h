#ifndef SC_MLKEM_H
#define SC_MLKEM_H

#include <stdint.h>

/* ML-KEM-768 (FIPS 203).  Deterministic internal forms: callers supply
 * the randomness (d || z for key generation, m for encapsulation) from
 * a CSPRNG.  Decapsulation uses implicit rejection. */

#define SC_MLKEM_EK 1184
#define SC_MLKEM_DK 2400
#define SC_MLKEM_CT 1088
#define SC_MLKEM_SS 32

void sc_mlkem768_keygen(uint8_t ek[SC_MLKEM_EK], uint8_t dk[SC_MLKEM_DK], const uint8_t seed[64]);
/* Returns -1 if ek fails the FIPS 203 modulus check. */
int  sc_mlkem768_encaps(uint8_t ct[SC_MLKEM_CT], uint8_t ss[SC_MLKEM_SS],
                        const uint8_t ek[SC_MLKEM_EK], const uint8_t m[32]);
void sc_mlkem768_decaps(uint8_t ss[SC_MLKEM_SS], const uint8_t ct[SC_MLKEM_CT],
                        const uint8_t dk[SC_MLKEM_DK]);

#endif
