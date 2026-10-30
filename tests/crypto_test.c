/* Host test suite for crypto/.  Checks every primitive against the
 * published RFC vectors and against vectors.h, which was produced by
 * independent implementations (tests/gen_vectors.py).  Run: make test */
#include <stdio.h>
#include <string.h>

#include "aead.h"
#include "chacha20.h"
#include "drbg.h"
#include "ed25519.h"
#include "poly1305.h"
#include "sha2.h"
#include "vectors.h"

static int failures, checks;

static void check(int ok, const char *what) {
    checks++;
    if (!ok) {
        failures++;
        printf("FAIL %s\n", what);
    }
}

static int unhex(unsigned char *out, const char *hex) {
    int n = 0;
    while (hex[0] && hex[1]) {
        unsigned v;
        sscanf(hex, "%2x", &v);
        out[n++] = (unsigned char)v;
        hex += 2;
    }
    return n;
}

#define N(a) (sizeof(a) / sizeof((a)[0]))

static void test_rfc(void) {
    unsigned char key[32], msg[128], exp[128], out[128], sig[64], pk[32];
    int n;

    /* RFC 8439 2.5.2: Poly1305 */
    unhex(key, "85d6be7857556d337f4452fe42d506a80103808afb0db2fd4abff6af4149f51b");
    unhex(exp, "a8061dc1305136c6c22b8baf0c0127a9");
    sc_poly1305(key, (const unsigned char *)"Cryptographic Forum Research Group", 34, out);
    check(memcmp(out, exp, 16) == 0, "rfc8439 poly1305");

    /* RFC 5869 A.1: HKDF-SHA-256 */
    {
        unsigned char ikm[22], salt[13], info[10];
        memset(ikm, 0x0b, sizeof(ikm));
        unhex(salt, "000102030405060708090a0b0c");
        unhex(info, "f0f1f2f3f4f5f6f7f8f9");
        unhex(exp, "3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf34007208d5b887185865");
        sc_hkdf(salt, 13, ikm, 22, info, 10, out, 42);
        check(memcmp(out, exp, 42) == 0, "rfc5869 hkdf A.1");
    }

    /* RFC 8032 7.1 tests 1 and 2: Ed25519 */
    unhex(key, "9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60");
    unhex(exp, "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a");
    sc_ed25519_public_key(pk, key);
    check(memcmp(pk, exp, 32) == 0, "rfc8032 test1 public key");
    unhex(exp, "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b");
    sc_ed25519_sign(sig, msg, 0, key, pk);
    check(memcmp(sig, exp, 64) == 0, "rfc8032 test1 signature");
    check(sc_ed25519_verify(sig, msg, 0, pk) == 0, "rfc8032 test1 verify");

    unhex(key, "4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb");
    unhex(exp, "3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c");
    sc_ed25519_public_key(pk, key);
    check(memcmp(pk, exp, 32) == 0, "rfc8032 test2 public key");
    n = unhex(msg, "72");
    unhex(exp, "92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00");
    sc_ed25519_sign(sig, msg, (size_t)n, key, pk);
    check(memcmp(sig, exp, 64) == 0, "rfc8032 test2 signature");
}

static void test_hashes(void) {
    unsigned char out[64], prk[32];
    char name[64];

    for (unsigned i = 0; i < N(hash_vecs); i++) {
        sc_sha256(hash_vecs[i].msg, hash_vecs[i].len, out);
        snprintf(name, sizeof(name), "sha256 len=%u", hash_vecs[i].len);
        check(memcmp(out, hash_vecs[i].sha256, 32) == 0, name);
        sc_sha512(hash_vecs[i].msg, hash_vecs[i].len, out);
        snprintf(name, sizeof(name), "sha512 len=%u", hash_vecs[i].len);
        check(memcmp(out, hash_vecs[i].sha512, 64) == 0, name);
    }

    /* Streaming in odd-sized pieces must match one-shot. */
    {
        const hash_vec_t *v = &hash_vecs[N(hash_vecs) - 1];
        sc_sha256_t st;
        sc_sha256_init(&st);
        for (unsigned o = 0; o < v->len; o += 7) sc_sha256_update(&st, v->msg + o, v->len - o < 7 ? v->len - o : 7);
        sc_sha256_final(&st, out);
        check(memcmp(out, v->sha256, 32) == 0, "sha256 streaming");
    }

    for (unsigned i = 0; i < N(hmac_vecs); i++) {
        sc_hmac_sha256(hmac_vecs[i].key, hmac_vecs[i].klen, hmac_vecs[i].msg, hmac_vecs[i].mlen, out);
        snprintf(name, sizeof(name), "hmac-sha256 klen=%u mlen=%u", hmac_vecs[i].klen, hmac_vecs[i].mlen);
        check(memcmp(out, hmac_vecs[i].mac, 32) == 0, name);
    }

    for (unsigned i = 0; i < N(hkdf_vecs); i++) {
        unsigned char okm[255];
        const hkdf_vec_t *v = &hkdf_vecs[i];
        sc_hkdf(v->salt, v->slen, v->ikm, hkdf_ikm_len[i], v->info, v->ilen, okm, v->olen);
        snprintf(name, sizeof(name), "hkdf olen=%u", v->olen);
        check(memcmp(okm, v->okm, v->olen) == 0, name);
    }
    check(sc_hkdf_expand(prk, 0, 0, out, 255 * 32 + 1) == -1, "hkdf rejects oversize output");
}

static void test_aead(const aead_vec_t *vecs, unsigned count, int x) {
    unsigned char ct[1024], pt[1024], tag[16];
    char name[64];

    for (unsigned i = 0; i < count; i++) {
        const aead_vec_t *v = &vecs[i];
        if (x) sc_xaead_seal(ct, tag, v->pt, v->mlen, v->aad, v->alen, v->nonce, v->key);
        else   sc_aead_seal(ct, tag, v->pt, v->mlen, v->aad, v->alen, v->nonce, v->key);
        snprintf(name, sizeof(name), "%s seal mlen=%u alen=%u", x ? "xchacha" : "chacha", v->mlen, v->alen);
        check(memcmp(ct, v->ct, v->mlen) == 0 && memcmp(tag, v->tag, 16) == 0, name);

        memset(pt, 0xaa, sizeof(pt));
        snprintf(name, sizeof(name), "%s open mlen=%u", x ? "xchacha" : "chacha", v->mlen);
        check((x ? sc_xaead_open(pt, v->ct, v->mlen, v->tag, v->aad, v->alen, v->nonce, v->key)
                 : sc_aead_open(pt, v->ct, v->mlen, v->tag, v->aad, v->alen, v->nonce, v->key)) == 0
              && memcmp(pt, v->pt, v->mlen) == 0, name);

        /* Any flipped bit in ciphertext, tag or AAD must be rejected, and
         * a rejected open must not write plaintext. */
        if (v->mlen > 0) {
            memcpy(ct, v->ct, v->mlen);
            ct[v->mlen / 2] ^= 1;
            memset(pt, 0xaa, sizeof(pt));
            snprintf(name, sizeof(name), "%s rejects tampered ct mlen=%u", x ? "xchacha" : "chacha", v->mlen);
            check((x ? sc_xaead_open(pt, ct, v->mlen, v->tag, v->aad, v->alen, v->nonce, v->key)
                     : sc_aead_open(pt, ct, v->mlen, v->tag, v->aad, v->alen, v->nonce, v->key)) == -1
                  && pt[0] == 0xaa, name);
        }
        memcpy(tag, v->tag, 16);
        tag[15] ^= 0x80;
        snprintf(name, sizeof(name), "%s rejects tampered tag", x ? "xchacha" : "chacha");
        check((x ? sc_xaead_open(pt, v->ct, v->mlen, tag, v->aad, v->alen, v->nonce, v->key)
                 : sc_aead_open(pt, v->ct, v->mlen, tag, v->aad, v->alen, v->nonce, v->key)) == -1, name);
    }
}

static void test_ed25519(void) {
    unsigned char pk[32], sig[64], bad[64];
    char name[64];

    for (unsigned i = 0; i < N(ed_vecs); i++) {
        const ed_vec_t *v = &ed_vecs[i];
        sc_ed25519_public_key(pk, v->seed);
        snprintf(name, sizeof(name), "ed25519 public key #%u", i);
        check(memcmp(pk, v->pk, 32) == 0, name);
        sc_ed25519_sign(sig, v->msg, v->mlen, v->seed, v->pk);
        snprintf(name, sizeof(name), "ed25519 sign mlen=%u", v->mlen);
        check(memcmp(sig, v->sig, 64) == 0, name);
        snprintf(name, sizeof(name), "ed25519 verify mlen=%u", v->mlen);
        check(sc_ed25519_verify(v->sig, v->msg, v->mlen, v->pk) == 0, name);

        memcpy(bad, v->sig, 64);
        bad[5] ^= 4;
        snprintf(name, sizeof(name), "ed25519 rejects bad R #%u", i);
        check(sc_ed25519_verify(bad, v->msg, v->mlen, v->pk) == -1, name);
        memcpy(bad, v->sig, 64);
        bad[40] ^= 1;
        snprintf(name, sizeof(name), "ed25519 rejects bad S #%u", i);
        check(sc_ed25519_verify(bad, v->msg, v->mlen, v->pk) == -1, name);
        if (v->mlen > 0) {
            unsigned char m[1024];
            memcpy(m, v->msg, v->mlen);
            m[0] ^= 1;
            snprintf(name, sizeof(name), "ed25519 rejects changed message #%u", i);
            check(sc_ed25519_verify(v->sig, m, v->mlen, v->pk) == -1, name);
        }
    }

    /* S >= L is non-canonical (malleability) and must be rejected. */
    memcpy(bad, ed_vecs[0].sig, 64);
    for (int i = 32; i < 64; i++) bad[i] = 0xff;
    bad[63] = 0x1f;
    check(sc_ed25519_verify(bad, ed_vecs[0].msg, ed_vecs[0].mlen, ed_vecs[0].pk) == -1,
          "ed25519 rejects non-canonical S");
}

static void test_drbg(void) {
    sc_drbg_t a = {{0}, 0}, b = {{0}, 0};
    unsigned char x[100], y[100], z[100];

    check(sc_drbg_generate(&a, x, sizeof(x)) == -1, "drbg refuses before seeding");
    sc_drbg_reseed(&a, (const unsigned char *)"seed", 4);
    sc_drbg_reseed(&b, (const unsigned char *)"seed", 4);
    sc_drbg_generate(&a, x, sizeof(x));
    sc_drbg_generate(&b, y, sizeof(y));
    check(memcmp(x, y, sizeof(x)) == 0, "drbg deterministic for equal seeds");
    sc_drbg_generate(&a, z, sizeof(z));
    check(memcmp(x, z, sizeof(x)) != 0, "drbg output does not repeat");
    sc_drbg_reseed(&b, (const unsigned char *)"more", 4);
    sc_drbg_generate(&b, y, sizeof(y));
    check(memcmp(y, z, sizeof(y)) != 0, "drbg reseed changes stream");
}

int main(void) {
    test_rfc();
    test_hashes();
    test_aead(ietf_vecs, N(ietf_vecs), 0);
    test_aead(x_vecs, N(x_vecs), 1);
    test_ed25519();
    test_drbg();
    printf("%d/%d checks passed\n", checks - failures, checks);
    return failures ? 1 : 0;
}
