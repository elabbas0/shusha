#include "sha2.h"
#include "sc.h"

/* ---- SHA-256 ----------------------------------------------------------- */

static const uint32_t K256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

#define ROR32(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha256_block(uint32_t h[8], const uint8_t p[64]) {
    uint32_t w[64], a, b, c, d, e, f, g, k, t1, t2;

    for (int i = 0; i < 16; i++) w[i] = sc_load32_be(p + 4 * i);
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ROR32(w[i - 15], 7) ^ ROR32(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ROR32(w[i - 2], 17) ^ ROR32(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    a = h[0]; b = h[1]; c = h[2]; d = h[3]; e = h[4]; f = h[5]; g = h[6]; k = h[7];
    for (int i = 0; i < 64; i++) {
        t1 = k + (ROR32(e, 6) ^ ROR32(e, 11) ^ ROR32(e, 25)) + ((e & f) ^ (~e & g)) + K256[i] + w[i];
        t2 = (ROR32(a, 2) ^ ROR32(a, 13) ^ ROR32(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        k = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += k;
    sc_wipe(w, sizeof(w));
}

void sc_sha256_init(sc_sha256_t *st) {
    static const uint32_t iv[8] = {
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19
    };
    for (int i = 0; i < 8; i++) st->h[i] = iv[i];
    st->len = 0;
    st->used = 0;
}

void sc_sha256_update(sc_sha256_t *st, const uint8_t *m, size_t len) {
    st->len += len;
    while (len--) {
        st->buf[st->used++] = *m++;
        if (st->used == 64) {
            sha256_block(st->h, st->buf);
            st->used = 0;
        }
    }
}

void sc_sha256_final(sc_sha256_t *st, uint8_t out[32]) {
    uint64_t bits = st->len * 8;
    st->buf[st->used++] = 0x80;
    if (st->used > 56) {
        while (st->used < 64) st->buf[st->used++] = 0;
        sha256_block(st->h, st->buf);
        st->used = 0;
    }
    while (st->used < 56) st->buf[st->used++] = 0;
    sc_store64_be(st->buf + 56, bits);
    sha256_block(st->h, st->buf);
    for (int i = 0; i < 8; i++) sc_store32_be(out + 4 * i, st->h[i]);
    sc_wipe(st, sizeof(*st));
}

void sc_sha256(const uint8_t *m, size_t len, uint8_t out[32]) {
    sc_sha256_t st;
    sc_sha256_init(&st);
    sc_sha256_update(&st, m, len);
    sc_sha256_final(&st, out);
}

/* ---- SHA-512 ----------------------------------------------------------- */

static const uint64_t K512[80] = {
    0x428a2f98d728ae22ULL, 0x7137449123ef65cdULL, 0xb5c0fbcfec4d3b2fULL, 0xe9b5dba58189dbbcULL,
    0x3956c25bf348b538ULL, 0x59f111f1b605d019ULL, 0x923f82a4af194f9bULL, 0xab1c5ed5da6d8118ULL,
    0xd807aa98a3030242ULL, 0x12835b0145706fbeULL, 0x243185be4ee4b28cULL, 0x550c7dc3d5ffb4e2ULL,
    0x72be5d74f27b896fULL, 0x80deb1fe3b1696b1ULL, 0x9bdc06a725c71235ULL, 0xc19bf174cf692694ULL,
    0xe49b69c19ef14ad2ULL, 0xefbe4786384f25e3ULL, 0x0fc19dc68b8cd5b5ULL, 0x240ca1cc77ac9c65ULL,
    0x2de92c6f592b0275ULL, 0x4a7484aa6ea6e483ULL, 0x5cb0a9dcbd41fbd4ULL, 0x76f988da831153b5ULL,
    0x983e5152ee66dfabULL, 0xa831c66d2db43210ULL, 0xb00327c898fb213fULL, 0xbf597fc7beef0ee4ULL,
    0xc6e00bf33da88fc2ULL, 0xd5a79147930aa725ULL, 0x06ca6351e003826fULL, 0x142929670a0e6e70ULL,
    0x27b70a8546d22ffcULL, 0x2e1b21385c26c926ULL, 0x4d2c6dfc5ac42aedULL, 0x53380d139d95b3dfULL,
    0x650a73548baf63deULL, 0x766a0abb3c77b2a8ULL, 0x81c2c92e47edaee6ULL, 0x92722c851482353bULL,
    0xa2bfe8a14cf10364ULL, 0xa81a664bbc423001ULL, 0xc24b8b70d0f89791ULL, 0xc76c51a30654be30ULL,
    0xd192e819d6ef5218ULL, 0xd69906245565a910ULL, 0xf40e35855771202aULL, 0x106aa07032bbd1b8ULL,
    0x19a4c116b8d2d0c8ULL, 0x1e376c085141ab53ULL, 0x2748774cdf8eeb99ULL, 0x34b0bcb5e19b48a8ULL,
    0x391c0cb3c5c95a63ULL, 0x4ed8aa4ae3418acbULL, 0x5b9cca4f7763e373ULL, 0x682e6ff3d6b2b8a3ULL,
    0x748f82ee5defb2fcULL, 0x78a5636f43172f60ULL, 0x84c87814a1f0ab72ULL, 0x8cc702081a6439ecULL,
    0x90befffa23631e28ULL, 0xa4506cebde82bde9ULL, 0xbef9a3f7b2c67915ULL, 0xc67178f2e372532bULL,
    0xca273eceea26619cULL, 0xd186b8c721c0c207ULL, 0xeada7dd6cde0eb1eULL, 0xf57d4f7fee6ed178ULL,
    0x06f067aa72176fbaULL, 0x0a637dc5a2c898a6ULL, 0x113f9804bef90daeULL, 0x1b710b35131c471bULL,
    0x28db77f523047d84ULL, 0x32caab7b40c72493ULL, 0x3c9ebe0a15c9bebcULL, 0x431d67c49c100d4cULL,
    0x4cc5d4becb3e42b6ULL, 0x597f299cfc657e2aULL, 0x5fcb6fab3ad6faecULL, 0x6c44198c4a475817ULL
};

#define ROR64(x, n) (((x) >> (n)) | ((x) << (64 - (n))))

static void sha512_block(uint64_t h[8], const uint8_t p[128]) {
    uint64_t w[80], a, b, c, d, e, f, g, k, t1, t2;

    for (int i = 0; i < 16; i++) w[i] = sc_load64_be(p + 8 * i);
    for (int i = 16; i < 80; i++) {
        uint64_t s0 = ROR64(w[i - 15], 1) ^ ROR64(w[i - 15], 8) ^ (w[i - 15] >> 7);
        uint64_t s1 = ROR64(w[i - 2], 19) ^ ROR64(w[i - 2], 61) ^ (w[i - 2] >> 6);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    a = h[0]; b = h[1]; c = h[2]; d = h[3]; e = h[4]; f = h[5]; g = h[6]; k = h[7];
    for (int i = 0; i < 80; i++) {
        t1 = k + (ROR64(e, 14) ^ ROR64(e, 18) ^ ROR64(e, 41)) + ((e & f) ^ (~e & g)) + K512[i] + w[i];
        t2 = (ROR64(a, 28) ^ ROR64(a, 34) ^ ROR64(a, 39)) + ((a & b) ^ (a & c) ^ (b & c));
        k = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += k;
    sc_wipe(w, sizeof(w));
}

void sc_sha512_init(sc_sha512_t *st) {
    static const uint64_t iv[8] = {
        0x6a09e667f3bcc908ULL, 0xbb67ae8584caa73bULL, 0x3c6ef372fe94f82bULL, 0xa54ff53a5f1d36f1ULL,
        0x510e527fade682d1ULL, 0x9b05688c2b3e6c1fULL, 0x1f83d9abfb41bd6bULL, 0x5be0cd19137e2179ULL
    };
    for (int i = 0; i < 8; i++) st->h[i] = iv[i];
    st->len = 0;
    st->used = 0;
}

void sc_sha512_update(sc_sha512_t *st, const uint8_t *m, size_t len) {
    st->len += len;
    while (len--) {
        st->buf[st->used++] = *m++;
        if (st->used == 128) {
            sha512_block(st->h, st->buf);
            st->used = 0;
        }
    }
}

void sc_sha512_final(sc_sha512_t *st, uint8_t out[64]) {
    uint64_t bits = st->len * 8;
    st->buf[st->used++] = 0x80;
    if (st->used > 112) {
        while (st->used < 128) st->buf[st->used++] = 0;
        sha512_block(st->h, st->buf);
        st->used = 0;
    }
    while (st->used < 120) st->buf[st->used++] = 0;
    sc_store64_be(st->buf + 120, bits);
    sha512_block(st->h, st->buf);
    for (int i = 0; i < 8; i++) sc_store64_be(out + 8 * i, st->h[i]);
    sc_wipe(st, sizeof(*st));
}

void sc_sha512(const uint8_t *m, size_t len, uint8_t out[64]) {
    sc_sha512_t st;
    sc_sha512_init(&st);
    sc_sha512_update(&st, m, len);
    sc_sha512_final(&st, out);
}

/* ---- HMAC-SHA-256 / HKDF ----------------------------------------------- */

void sc_hmac_sha256(const uint8_t *key, size_t key_len,
                    const uint8_t *m, size_t len, uint8_t out[32]) {
    uint8_t k[64], pad[64], inner[32];
    sc_sha256_t st;

    for (int i = 0; i < 64; i++) k[i] = 0;
    if (key_len > 64) {
        sc_sha256(key, key_len, k);
    } else {
        for (size_t i = 0; i < key_len; i++) k[i] = key[i];
    }

    for (int i = 0; i < 64; i++) pad[i] = k[i] ^ 0x36;
    sc_sha256_init(&st);
    sc_sha256_update(&st, pad, 64);
    sc_sha256_update(&st, m, len);
    sc_sha256_final(&st, inner);

    for (int i = 0; i < 64; i++) pad[i] = k[i] ^ 0x5c;
    sc_sha256_init(&st);
    sc_sha256_update(&st, pad, 64);
    sc_sha256_update(&st, inner, 32);
    sc_sha256_final(&st, out);

    sc_wipe(k, sizeof(k));
    sc_wipe(pad, sizeof(pad));
    sc_wipe(inner, sizeof(inner));
}

void sc_hkdf_extract(const uint8_t *salt, size_t salt_len,
                     const uint8_t *ikm, size_t ikm_len, uint8_t prk[32]) {
    static const uint8_t zero_salt[32];
    if (!salt || salt_len == 0) {
        salt = zero_salt;
        salt_len = 32;
    }
    sc_hmac_sha256(salt, salt_len, ikm, ikm_len, prk);
}

/* T(i) = HMAC(PRK, T(i-1) || info || i).  The HMAC is computed inline
 * so T(i-1) || info || i never needs a variable-size buffer. */
int sc_hkdf_expand(const uint8_t prk[32], const uint8_t *info, size_t info_len,
                   uint8_t *out, size_t out_len) {
    uint8_t t[32], pad[64], inner[32];
    size_t t_len = 0, done = 0;
    uint8_t counter = 1;
    sc_sha256_t st;

    if (out_len > 255 * 32) return -1;
    while (done < out_len) {
        for (int i = 0; i < 64; i++) pad[i] = (uint8_t)((i < 32 ? prk[i] : 0) ^ 0x36);
        sc_sha256_init(&st);
        sc_sha256_update(&st, pad, 64);
        sc_sha256_update(&st, t, t_len);
        sc_sha256_update(&st, info, info_len);
        sc_sha256_update(&st, &counter, 1);
        sc_sha256_final(&st, inner);
        for (int i = 0; i < 64; i++) pad[i] = (uint8_t)((i < 32 ? prk[i] : 0) ^ 0x5c);
        sc_sha256_init(&st);
        sc_sha256_update(&st, pad, 64);
        sc_sha256_update(&st, inner, 32);
        sc_sha256_final(&st, t);
        t_len = 32;
        for (size_t i = 0; i < 32 && done < out_len; i++) out[done++] = t[i];
        counter++;
    }
    sc_wipe(t, sizeof(t));
    sc_wipe(pad, sizeof(pad));
    sc_wipe(inner, sizeof(inner));
    return 0;
}

int sc_hkdf(const uint8_t *salt, size_t salt_len,
            const uint8_t *ikm, size_t ikm_len,
            const uint8_t *info, size_t info_len,
            uint8_t *out, size_t out_len) {
    uint8_t prk[32];
    int r;
    sc_hkdf_extract(salt, salt_len, ikm, ikm_len, prk);
    r = sc_hkdf_expand(prk, info, info_len, out, out_len);
    sc_wipe(prk, sizeof(prk));
    return r;
}
