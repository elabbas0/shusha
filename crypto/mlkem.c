/* ML-KEM-768 following FIPS 203 algorithm by algorithm (Alg. 4-21).
 * Clarity over speed: coefficients are kept fully reduced in [0, q). */
#include "mlkem.h"
#include "keccak.h"
#include "sc.h"

#define N 256
#define Q 3329
#define K 3
#define ETA1 2
#define ETA2 2
#define DU 10
#define DV 4

typedef struct { uint16_t c[N]; } poly;

/* zeta^BitRev7(i) for the NTT, and the base-case gammas. */
static uint16_t zetas[128], gammas[128];
static int tables_ready;

static uint32_t powmod(uint32_t b, uint32_t e) {
    uint32_t r = 1;
    while (e) {
        if (e & 1) r = r * b % Q;
        b = b * b % Q;
        e >>= 1;
    }
    return r;
}

static uint32_t bitrev7(uint32_t i) {
    uint32_t r = 0;
    for (int b = 0; b < 7; b++) r |= ((i >> b) & 1) << (6 - b);
    return r;
}

static void init_tables(void) {
    if (tables_ready) return;
    for (uint32_t i = 0; i < 128; i++) {
        zetas[i] = (uint16_t)powmod(17, bitrev7(i));
        gammas[i] = (uint16_t)powmod(17, 2 * bitrev7(i) + 1);
    }
    tables_ready = 1;
}

static uint16_t addq(uint32_t a, uint32_t b) { return (uint16_t)((a + b) % Q); }
static uint16_t subq(uint32_t a, uint32_t b) { return (uint16_t)((a + Q - b) % Q); }
static uint16_t mulq(uint32_t a, uint32_t b) { return (uint16_t)(a * b % Q); }

/* Algorithm 9 */
static void ntt(poly *f) {
    int i = 1;
    for (int len = 128; len >= 2; len /= 2) {
        for (int start = 0; start < N; start += 2 * len) {
            uint32_t z = zetas[i++];
            for (int j = start; j < start + len; j++) {
                uint16_t t = mulq(z, f->c[j + len]);
                f->c[j + len] = subq(f->c[j], t);
                f->c[j] = addq(f->c[j], t);
            }
        }
    }
}

/* Algorithm 10 */
static void ntt_inv(poly *f) {
    int i = 127;
    for (int len = 2; len <= 128; len *= 2) {
        for (int start = 0; start < N; start += 2 * len) {
            uint32_t z = zetas[i--];
            for (int j = start; j < start + len; j++) {
                uint16_t t = f->c[j];
                f->c[j] = addq(t, f->c[j + len]);
                f->c[j + len] = mulq(z, subq(f->c[j + len], t));
            }
        }
    }
    for (int j = 0; j < N; j++) f->c[j] = mulq(f->c[j], 3303); /* 128^-1 mod q */
}

/* Algorithms 11-12: h += f * g in the NTT domain. */
static void mul_acc(poly *h, const poly *f, const poly *g) {
    for (int i = 0; i < 128; i++) {
        uint32_t a0 = f->c[2 * i], a1 = f->c[2 * i + 1], b0 = g->c[2 * i], b1 = g->c[2 * i + 1];
        uint16_t c0 = addq(mulq(a0, b0), mulq(mulq(a1, b1), gammas[i]));
        uint16_t c1 = addq(mulq(a0, b1), mulq(a1, b0));
        h->c[2 * i] = addq(h->c[2 * i], c0);
        h->c[2 * i + 1] = addq(h->c[2 * i + 1], c1);
    }
}

/* Algorithm 7: rejection-sample a polynomial in the NTT domain. */
static void sample_ntt(poly *a, const uint8_t rho[32], uint8_t j, uint8_t i) {
    sc_keccak_t xof;
    uint8_t seed[34], c[3];
    int n = 0;
    for (int k = 0; k < 32; k++) seed[k] = rho[k];
    seed[32] = j;
    seed[33] = i;
    sc_keccak_init(&xof, SC_SHAKE128_RATE, 0x1f);
    sc_keccak_absorb(&xof, seed, 34);
    sc_keccak_finalize(&xof);
    while (n < N) {
        uint32_t d1, d2;
        sc_keccak_squeeze(&xof, c, 3);
        d1 = c[0] + 256u * (c[1] & 15u);
        d2 = (c[1] >> 4) + 16u * c[2];
        if (d1 < Q) a->c[n++] = (uint16_t)d1;
        if (d2 < Q && n < N) a->c[n++] = (uint16_t)d2;
    }
}

/* Algorithm 8 with PRF_eta(s, b) = SHAKE256(s || b, 64 * eta). */
static void sample_cbd(poly *f, const uint8_t s[32], uint8_t b, int eta) {
    uint8_t in[33], buf[64 * 3];
    for (int k = 0; k < 32; k++) in[k] = s[k];
    in[32] = b;
    sc_shake256(in, 33, buf, (size_t)64 * eta);
    for (int i = 0; i < N; i++) {
        uint32_t x = 0, y = 0;
        for (int j = 0; j < eta; j++) {
            int bx = 2 * i * eta + j, by = 2 * i * eta + eta + j;
            x += (buf[bx >> 3] >> (bx & 7)) & 1;
            y += (buf[by >> 3] >> (by & 7)) & 1;
        }
        f->c[i] = subq(x, y);
    }
    sc_wipe(buf, sizeof(buf));
}

/* Algorithms 5-6: d-bit little-endian packing. */
static void encode(uint8_t *out, const poly *f, int d) {
    uint32_t acc = 0;
    int bits = 0, o = 0;
    for (int i = 0; i < N; i++) {
        acc |= (uint32_t)f->c[i] << bits;
        bits += d;
        while (bits >= 8) {
            out[o++] = (uint8_t)acc;
            acc >>= 8;
            bits -= 8;
        }
    }
}

static void decode(poly *f, const uint8_t *in, int d) {
    uint32_t acc = 0, mask = (1u << d) - 1;
    int bits = 0, o = 0;
    for (int i = 0; i < N; i++) {
        while (bits < d) {
            acc |= (uint32_t)in[o++] << bits;
            bits += 8;
        }
        f->c[i] = (uint16_t)(acc & mask);
        acc >>= d;
        bits -= d;
        if (d == 12) f->c[i] %= Q;
    }
}

static uint16_t compress(uint32_t x, int d) {
    return (uint16_t)((((x << d) + Q / 2) / Q) & ((1u << d) - 1));
}

static uint16_t decompress(uint32_t y, int d) {
    return (uint16_t)((y * Q + (1u << (d - 1))) >> d);
}

/* Algorithm 13: K-PKE.KeyGen */
static void pke_keygen(uint8_t ek[SC_MLKEM_EK], uint8_t dkp[384 * K], const uint8_t d[32]) {
    uint8_t in[33], g[64];
    poly s[K], e[K], t, a;
    for (int i = 0; i < 32; i++) in[i] = d[i];
    in[32] = K;
    sc_sha3_512(in, 33, g); /* rho = g[0..32), sigma = g[32..64) */
    for (int i = 0; i < K; i++) {
        sample_cbd(&s[i], g + 32, (uint8_t)i, ETA1);
        ntt(&s[i]);
    }
    for (int i = 0; i < K; i++) {
        sample_cbd(&e[i], g + 32, (uint8_t)(K + i), ETA1);
        ntt(&e[i]);
    }
    for (int i = 0; i < K; i++) {
        t = e[i];
        for (int j = 0; j < K; j++) {
            sample_ntt(&a, g, (uint8_t)j, (uint8_t)i);
            mul_acc(&t, &a, &s[j]);
        }
        encode(ek + 384 * i, &t, 12);
        encode(dkp + 384 * i, &s[i], 12);
    }
    for (int i = 0; i < 32; i++) ek[384 * K + i] = g[i];
    sc_wipe(s, sizeof(s));
    sc_wipe(e, sizeof(e));
    sc_wipe(g, sizeof(g));
}

/* Algorithm 14: K-PKE.Encrypt */
static void pke_encrypt(uint8_t ct[SC_MLKEM_CT], const uint8_t ek[SC_MLKEM_EK],
                        const uint8_t m[32], const uint8_t r[32]) {
    const uint8_t *rho = ek + 384 * K;
    poly t[K], y[K], u, v, e, a, mu;
    for (int i = 0; i < K; i++) decode(&t[i], ek + 384 * i, 12);
    for (int i = 0; i < K; i++) {
        sample_cbd(&y[i], r, (uint8_t)i, ETA1);
        ntt(&y[i]);
    }
    for (int i = 0; i < K; i++) {
        for (int k = 0; k < N; k++) u.c[k] = 0;
        for (int j = 0; j < K; j++) {
            sample_ntt(&a, rho, (uint8_t)i, (uint8_t)j); /* (A^T)[i][j] = A[j][i] */
            mul_acc(&u, &a, &y[j]);
        }
        ntt_inv(&u);
        sample_cbd(&e, r, (uint8_t)(K + i), ETA2);
        for (int k = 0; k < N; k++) u.c[k] = compress(addq(u.c[k], e.c[k]), DU);
        encode(ct + 320 * i, &u, DU);
    }
    for (int k = 0; k < N; k++) v.c[k] = 0;
    for (int i = 0; i < K; i++) mul_acc(&v, &t[i], &y[i]);
    ntt_inv(&v);
    sample_cbd(&e, r, (uint8_t)(2 * K), ETA2);
    for (int k = 0; k < N; k++) {
        mu.c[k] = decompress((m[k >> 3] >> (k & 7)) & 1, 1);
        v.c[k] = compress(addq(addq(v.c[k], e.c[k]), mu.c[k]), DV);
    }
    encode(ct + 320 * K, &v, DV);
    sc_wipe(y, sizeof(y));
    sc_wipe(&e, sizeof(e));
    sc_wipe(&mu, sizeof(mu));
}

/* Algorithm 15: K-PKE.Decrypt */
static void pke_decrypt(uint8_t m[32], const uint8_t dkp[384 * K], const uint8_t ct[SC_MLKEM_CT]) {
    poly u, v, s, w;
    for (int k = 0; k < N; k++) w.c[k] = 0;
    for (int i = 0; i < K; i++) {
        decode(&u, ct + 320 * i, DU);
        for (int k = 0; k < N; k++) u.c[k] = decompress(u.c[k], DU);
        ntt(&u);
        decode(&s, dkp + 384 * i, 12);
        mul_acc(&w, &s, &u);
    }
    ntt_inv(&w);
    decode(&v, ct + 320 * K, DV);
    for (int i = 0; i < 32; i++) m[i] = 0;
    for (int k = 0; k < N; k++) {
        uint16_t x = subq(decompress(v.c[k], DV), w.c[k]);
        m[k >> 3] |= (uint8_t)(compress(x, 1) << (k & 7));
    }
    sc_wipe(&s, sizeof(s));
    sc_wipe(&w, sizeof(w));
}

/* Algorithm 16 (ML-KEM.KeyGen_internal) */
void sc_mlkem768_keygen(uint8_t ek[SC_MLKEM_EK], uint8_t dk[SC_MLKEM_DK], const uint8_t seed[64]) {
    init_tables();
    pke_keygen(ek, dk, seed);
    for (int i = 0; i < SC_MLKEM_EK; i++) dk[384 * K + i] = ek[i];
    sc_sha3_256(ek, SC_MLKEM_EK, dk + 384 * K + SC_MLKEM_EK);
    for (int i = 0; i < 32; i++) dk[SC_MLKEM_DK - 32 + i] = seed[32 + i];
}

/* Algorithm 17 (Encaps_internal) with the encapsulation-key check. */
int sc_mlkem768_encaps(uint8_t ct[SC_MLKEM_CT], uint8_t ss[SC_MLKEM_SS],
                       const uint8_t ek[SC_MLKEM_EK], const uint8_t m[32]) {
    uint8_t in[64], kr[64], check[384];
    poly t;
    init_tables();
    for (int i = 0; i < K; i++) {
        decode(&t, ek + 384 * i, 12);
        encode(check, &t, 12);
        for (int j = 0; j < 384; j++) {
            if (check[j] != ek[384 * i + j]) return -1;
        }
    }
    for (int i = 0; i < 32; i++) in[i] = m[i];
    sc_sha3_256(ek, SC_MLKEM_EK, in + 32);
    sc_sha3_512(in, 64, kr);
    pke_encrypt(ct, ek, m, kr + 32);
    for (int i = 0; i < 32; i++) ss[i] = kr[i];
    sc_wipe(kr, sizeof(kr));
    sc_wipe(in, sizeof(in));
    return 0;
}

/* Algorithm 18 (Decaps_internal), implicit rejection in constant time. */
void sc_mlkem768_decaps(uint8_t ss[SC_MLKEM_SS], const uint8_t ct[SC_MLKEM_CT],
                        const uint8_t dk[SC_MLKEM_DK]) {
    const uint8_t *dkp = dk, *ek = dk + 384 * K, *h = ek + SC_MLKEM_EK, *z = h + 32;
    uint8_t m[32], in[64], kr[64], kbar[32], ct2[SC_MLKEM_CT];
    sc_keccak_t j;
    uint8_t diff = 0, mask;

    init_tables();
    pke_decrypt(m, dkp, ct);
    for (int i = 0; i < 32; i++) {
        in[i] = m[i];
        in[32 + i] = h[i];
    }
    sc_sha3_512(in, 64, kr);
    sc_keccak_init(&j, SC_SHAKE256_RATE, 0x1f);
    sc_keccak_absorb(&j, z, 32);
    sc_keccak_absorb(&j, ct, SC_MLKEM_CT);
    sc_keccak_finalize(&j);
    sc_keccak_squeeze(&j, kbar, 32);
    pke_encrypt(ct2, ek, m, kr + 32);
    for (int i = 0; i < SC_MLKEM_CT; i++) diff |= ct[i] ^ ct2[i];
    mask = (uint8_t)(((uint32_t)diff - 1) >> 8); /* 0xff if equal, 0 if not */
    for (int i = 0; i < 32; i++) ss[i] = (uint8_t)((kr[i] & mask) | (kbar[i] & ~mask));
    sc_wipe(m, sizeof(m));
    sc_wipe(kr, sizeof(kr));
    sc_wipe(kbar, sizeof(kbar));
    sc_wipe(in, sizeof(in));
}
