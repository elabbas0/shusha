#include "argon2.h"
#include "blake2b.h"
#include "sc.h"

#define QW 128 /* 64-bit words per block */

typedef struct { uint64_t v[QW]; } block_t;

static void le32(uint8_t *p, uint32_t v) { sc_store32_le(p, v); }

/* H' (RFC 9106, 3.3): variable-length hash built on BLAKE2b. */
static void hprime(uint8_t *out, uint32_t outlen, const uint8_t *in, size_t inlen) {
    uint8_t len[4], v[64];
    sc_blake2b_t st;
    le32(len, outlen);
    if (outlen <= 64) {
        sc_blake2b_init(&st, outlen, 0, 0);
        sc_blake2b_update(&st, len, 4);
        sc_blake2b_update(&st, in, inlen);
        sc_blake2b_final(&st, out);
        return;
    }
    sc_blake2b_init(&st, 64, 0, 0);
    sc_blake2b_update(&st, len, 4);
    sc_blake2b_update(&st, in, inlen);
    sc_blake2b_final(&st, v);
    for (int i = 0; i < 32; i++) out[i] = v[i];
    out += 32;
    outlen -= 32;
    while (outlen > 64) {
        sc_blake2b(v, 64, 0, 0, v, 64);
        for (int i = 0; i < 32; i++) out[i] = v[i];
        out += 32;
        outlen -= 32;
    }
    sc_blake2b(v, outlen, 0, 0, v, 64);
    for (uint32_t i = 0; i < outlen; i++) out[i] = v[i];
    sc_wipe(v, sizeof(v));
}

#define ROR64(v, n) (((v) >> (n)) | ((v) << (64 - (n))))
static inline uint64_t fBlaMka(uint64_t a, uint64_t b) {
    return a + b + 2 * (a & 0xffffffffULL) * (b & 0xffffffffULL);
}
#define GB(a, b, c, d)                                  \
    a = fBlaMka(a, b); d = ROR64(d ^ a, 32);            \
    c = fBlaMka(c, d); b = ROR64(b ^ c, 24);            \
    a = fBlaMka(a, b); d = ROR64(d ^ a, 16);            \
    c = fBlaMka(c, d); b = ROR64(b ^ c, 63)

static void P(uint64_t *v0, uint64_t *v1, uint64_t *v2, uint64_t *v3, uint64_t *v4, uint64_t *v5,
              uint64_t *v6, uint64_t *v7, uint64_t *v8, uint64_t *v9, uint64_t *v10, uint64_t *v11,
              uint64_t *v12, uint64_t *v13, uint64_t *v14, uint64_t *v15) {
    GB(*v0, *v4, *v8, *v12);
    GB(*v1, *v5, *v9, *v13);
    GB(*v2, *v6, *v10, *v14);
    GB(*v3, *v7, *v11, *v15);
    GB(*v0, *v5, *v10, *v15);
    GB(*v1, *v6, *v11, *v12);
    GB(*v2, *v7, *v8, *v13);
    GB(*v3, *v4, *v9, *v14);
}

/* G(X, Y), optionally XORed into the existing contents of `out`. */
static void compress(block_t *out, const block_t *x, const block_t *y, int with_xor) {
    block_t r, z;
    for (int i = 0; i < QW; i++) r.v[i] = z.v[i] = x->v[i] ^ y->v[i];
    for (int i = 0; i < 8; i++) {
        uint64_t *q = z.v + 16 * i;
        P(&q[0], &q[1], &q[2], &q[3], &q[4], &q[5], &q[6], &q[7],
          &q[8], &q[9], &q[10], &q[11], &q[12], &q[13], &q[14], &q[15]);
    }
    for (int i = 0; i < 8; i++) {
        uint64_t *q = z.v + 2 * i;
        P(&q[0], &q[1], &q[16], &q[17], &q[32], &q[33], &q[48], &q[49],
          &q[64], &q[65], &q[80], &q[81], &q[96], &q[97], &q[112], &q[113]);
    }
    for (int i = 0; i < QW; i++) {
        uint64_t v = z.v[i] ^ r.v[i];
        out->v[i] = with_xor ? out->v[i] ^ v : v;
    }
    sc_wipe(&r, sizeof(r));
    sc_wipe(&z, sizeof(z));
}

/* Data-independent addressing: next block of 128 pseudo-random words. */
static void next_addresses(block_t *addr, block_t *input, const block_t *zero) {
    input->v[6]++;
    compress(addr, zero, input, 0);
    compress(addr, zero, addr, 0);
}

static void block_from_bytes(block_t *b, const uint8_t *in) {
    for (int i = 0; i < QW; i++) {
        b->v[i] = (uint64_t)sc_load32_le(in + 8 * i) | ((uint64_t)sc_load32_le(in + 8 * i + 4) << 32);
    }
}

static void block_to_bytes(uint8_t *out, const block_t *b) {
    for (int i = 0; i < QW; i++) sc_store64_le(out + 8 * i, b->v[i]);
}

int sc_argon2id(uint8_t *out, uint32_t outlen,
                const uint8_t *pwd, uint32_t pwdlen,
                const uint8_t *salt, uint32_t saltlen,
                const uint8_t *secret, uint32_t secretlen,
                const uint8_t *ad, uint32_t adlen,
                uint32_t passes, uint32_t mem_kib, uint32_t lanes,
                void *work) {
    block_t *B = (block_t *)work;
    uint32_t m, lane_len, seg_len;
    uint8_t h0[72], tmp[4], bytes[SC_ARGON2_BLOCK];
    sc_blake2b_t st;

    if (!work || outlen < 4 || passes < 1 || lanes < 1 || saltlen < 8) return -1;
    if (mem_kib < 8 * lanes) mem_kib = 8 * lanes;
    m = mem_kib / (4 * lanes) * 4 * lanes;
    lane_len = m / lanes;
    seg_len = lane_len / 4;

    /* H0 */
    sc_blake2b_init(&st, 64, 0, 0);
    {
        uint32_t params[6] = { lanes, outlen, mem_kib, passes, 0x13, 2 };
        for (int i = 0; i < 6; i++) {
            le32(tmp, params[i]);
            sc_blake2b_update(&st, tmp, 4);
        }
    }
    le32(tmp, pwdlen); sc_blake2b_update(&st, tmp, 4); sc_blake2b_update(&st, pwd, pwdlen);
    le32(tmp, saltlen); sc_blake2b_update(&st, tmp, 4); sc_blake2b_update(&st, salt, saltlen);
    le32(tmp, secretlen); sc_blake2b_update(&st, tmp, 4); sc_blake2b_update(&st, secret, secretlen);
    le32(tmp, adlen); sc_blake2b_update(&st, tmp, 4); sc_blake2b_update(&st, ad, adlen);
    sc_blake2b_final(&st, h0);

    /* First two blocks of each lane. */
    for (uint32_t l = 0; l < lanes; l++) {
        for (uint32_t c = 0; c < 2; c++) {
            le32(h0 + 64, c);
            le32(h0 + 68, l);
            hprime(bytes, SC_ARGON2_BLOCK, h0, 72);
            block_from_bytes(&B[l * lane_len + c], bytes);
        }
    }

    for (uint32_t pass = 0; pass < passes; pass++) {
        for (uint32_t slice = 0; slice < 4; slice++) {
            for (uint32_t lane = 0; lane < lanes; lane++) {
                /* Argon2id: data-independent addressing in the first half
                 * of the first pass, data-dependent afterwards. */
                int indep = pass == 0 && slice < 2;
                block_t zero, input, addr;
                uint32_t start = (pass == 0 && slice == 0) ? 2 : 0;

                if (indep) {
                    for (int i = 0; i < QW; i++) zero.v[i] = input.v[i] = 0;
                    input.v[0] = pass;
                    input.v[1] = lane;
                    input.v[2] = slice;
                    input.v[3] = m;
                    input.v[4] = passes;
                    input.v[5] = 2;
                    if (start) next_addresses(&addr, &input, &zero);
                }
                for (uint32_t idx = start; idx < seg_len; idx++) {
                    uint32_t cur = lane * lane_len + slice * seg_len + idx;
                    uint32_t prev = (slice * seg_len + idx == 0) ? lane * lane_len + lane_len - 1 : cur - 1;
                    uint64_t rnd;
                    uint32_t ref_lane, area, pos;
                    uint64_t x, y;

                    if (indep) {
                        if (idx % QW == 0) next_addresses(&addr, &input, &zero);
                        rnd = addr.v[idx % QW];
                    } else {
                        rnd = B[prev].v[0];
                    }

                    ref_lane = (uint32_t)((rnd >> 32) % lanes);
                    if (pass == 0 && slice == 0) ref_lane = lane;
                    if (ref_lane == lane) {
                        area = pass == 0 ? slice * seg_len + idx - 1 : lane_len - seg_len + idx - 1;
                    } else {
                        area = pass == 0 ? slice * seg_len - (idx == 0 ? 1 : 0)
                                         : lane_len - seg_len - (idx == 0 ? 1 : 0);
                    }
                    x = (rnd & 0xffffffffULL) * (rnd & 0xffffffffULL) >> 32;
                    y = (uint64_t)area * x >> 32;
                    pos = area - 1 - (uint32_t)y;
                    if (pass != 0 && slice != 3) pos += (slice + 1) * seg_len;
                    pos %= lane_len;

                    compress(&B[cur], &B[prev], &B[ref_lane * lane_len + pos], pass != 0);
                }
                if (indep) {
                    sc_wipe(&addr, sizeof(addr));
                    sc_wipe(&input, sizeof(input));
                }
            }
        }
    }

    /* Final block: XOR of the last column, then H'. */
    {
        block_t c = B[lane_len - 1];
        for (uint32_t l = 1; l < lanes; l++) {
            for (int i = 0; i < QW; i++) c.v[i] ^= B[l * lane_len + lane_len - 1].v[i];
        }
        block_to_bytes(bytes, &c);
        hprime(out, outlen, bytes, SC_ARGON2_BLOCK);
        sc_wipe(&c, sizeof(c));
    }
    sc_wipe(bytes, sizeof(bytes));
    sc_wipe(h0, sizeof(h0));
    sc_wipe(work, (size_t)m * SC_ARGON2_BLOCK);
    return 0;
}
