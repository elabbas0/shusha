/* Host micro-benchmark for crypto/ (numbers for docs; not a test). */
#include <stdio.h>
#include <time.h>
#include "aead.h"
#include "ed25519.h"
#include "sha2.h"

static double now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

static unsigned char buf[1 << 20];

int main(void) {
    unsigned char key[32] = {1}, nonce[24] = {2}, tag[16], seed[32] = {3}, pk[32], sig[64], out[32];
    double t;
    int i, n;

    n = 200;
    t = now();
    for (i = 0; i < n; i++) sc_xaead_seal(buf, tag, buf, sizeof(buf), 0, 0, nonce, key);
    printf("xchacha20-poly1305 seal: %.0f MB/s\n", n * (sizeof(buf) / 1048576.0) / (now() - t));
    t = now();
    for (i = 0; i < n; i++) sc_sha256(buf, sizeof(buf), out);
    printf("sha-256: %.0f MB/s\n", n * (sizeof(buf) / 1048576.0) / (now() - t));

    sc_ed25519_public_key(pk, seed);
    n = 200;
    t = now();
    for (i = 0; i < n; i++) sc_ed25519_sign(sig, buf, 64, seed, pk);
    printf("ed25519 sign: %.0f /s\n", n / (now() - t));
    t = now();
    for (i = 0; i < n; i++) (void)sc_ed25519_verify(sig, buf, 64, pk);
    printf("ed25519 verify: %.0f /s\n", n / (now() - t));
    return 0;
}
