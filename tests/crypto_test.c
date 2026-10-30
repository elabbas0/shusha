/* Host test suite for crypto/.  Checks every primitive against the
 * published RFC vectors and against vectors.h, which was produced by
 * independent implementations (tests/gen_vectors.py).  Run: make test */
#include <stdio.h>
#include <string.h>

#include <stdlib.h>
#include "aead.h"
#include "argon2.h"
#include "blake2b.h"
#include "keccak.h"
#include "mlkem.h"
#include "wrap.h"
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

static void test_keccak_blake2b(void) {
    unsigned char out[64];
    char name[64];
    for (unsigned i = 0; i < N(k_vecs); i++) {
        const k_vec_t *v = &k_vecs[i];
        sc_sha3_256(v->msg, v->len, out);
        snprintf(name, sizeof(name), "sha3-256 len=%u", v->len);
        check(memcmp(out, v->sha3_256, 32) == 0, name);
        sc_sha3_512(v->msg, v->len, out);
        snprintf(name, sizeof(name), "sha3-512 len=%u", v->len);
        check(memcmp(out, v->sha3_512, 64) == 0, name);
        sc_shake128(v->msg, v->len, out, 64);
        snprintf(name, sizeof(name), "shake128 len=%u", v->len);
        check(memcmp(out, v->shake128, 64) == 0, name);
        sc_shake256(v->msg, v->len, out, 64);
        snprintf(name, sizeof(name), "shake256 len=%u", v->len);
        check(memcmp(out, v->shake256, 64) == 0, name);
        sc_blake2b(out, 64, 0, 0, v->msg, v->len);
        snprintf(name, sizeof(name), "blake2b len=%u", v->len);
        check(memcmp(out, v->b2b, 64) == 0, name);
        sc_blake2b(out, 32, b2b_key, 32, v->msg, v->len);
        snprintf(name, sizeof(name), "blake2b keyed len=%u", v->len);
        check(memcmp(out, v->b2b_keyed, 32) == 0, name);
    }
}

static void test_argon2(void) {
    unsigned char out[64], pwd[32], salt[16], secret[8], ad[12], exp[32];
    char name[64];
    void *mem;

    /* RFC 9106 5.3: Argon2id, 4 lanes, with secret and associated data */
    memset(pwd, 1, 32);
    memset(salt, 2, 16);
    memset(secret, 3, 8);
    memset(ad, 4, 12);
    unhex(exp, "0d640df58d78766c08c037a34a8b53c9d01ef0452d75b65eb52520e96b01e659");
    mem = malloc(sc_argon2_mem_bytes(32, 4));
    sc_argon2id(out, 32, pwd, 32, salt, 16, secret, 8, ad, 12, 3, 32, 4, mem);
    free(mem);
    check(memcmp(out, exp, 32) == 0, "rfc9106 argon2id");

    for (unsigned i = 0; i < N(a2_vecs); i++) {
        const a2_vec_t *v = &a2_vecs[i];
        mem = malloc(sc_argon2_mem_bytes(v->m_kib, 1));
        sc_argon2id(out, v->outlen, (const unsigned char *)v->pwd, (unsigned)strlen(v->pwd), v->salt, 16,
                    0, 0, 0, 0, v->t, v->m_kib, 1, mem);
        free(mem);
        snprintf(name, sizeof(name), "argon2id t=%u m=%uKiB", v->t, v->m_kib);
        check(memcmp(out, v->out, v->outlen) == 0, name);
    }
}

static void test_x25519(void) {
    unsigned char k[32], u[32], out[32], exp[32];
    char name[64];
    /* RFC 7748 5.2 and 6.1 */
    unhex(k, "a546e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449ac4");
    unhex(u, "e6db6867583030db3594c1a424b15f7c726624ec26b3353b10a903a6d0ab1c4c");
    unhex(exp, "c3da55379de9c6908e94ea4df28d084f32eccf03491c71f754b4075577a28552");
    sc_x25519(out, k, u);
    check(memcmp(out, exp, 32) == 0, "rfc7748 x25519 5.2");
    unhex(k, "77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a");
    unhex(exp, "8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a");
    sc_x25519_base(out, k);
    check(memcmp(out, exp, 32) == 0, "rfc7748 x25519 6.1 public key");
    for (unsigned i = 0; i < N(x25519_vecs); i++) {
        const x_vec_t *v = &x25519_vecs[i];
        sc_x25519_base(out, v->a);
        snprintf(name, sizeof(name), "x25519 public key #%u", i);
        check(memcmp(out, v->a_pub, 32) == 0, name);
        sc_x25519(out, v->a, v->b_pub);
        snprintf(name, sizeof(name), "x25519 shared secret #%u", i);
        check(memcmp(out, v->shared, 32) == 0, name);
    }
}

static unsigned char ek[SC_MLKEM_EK], dk[SC_MLKEM_DK];

static void test_mlkem(void) {
    unsigned char h[32], ss[32], ss2[32], ct[SC_MLKEM_CT], m[32];
    char name[64];
    for (unsigned i = 0; i < N(mlkem_vecs); i++) {
        const mlkem_vec_t *v = &mlkem_vecs[i];
        sc_mlkem768_keygen(ek, dk, v->seed);
        sc_sha3_256(ek, SC_MLKEM_EK, h);
        snprintf(name, sizeof(name), "ml-kem-768 keygen #%u", i);
        check(memcmp(h, v->ek_sha3, 32) == 0, name);
        sc_mlkem768_decaps(ss, v->ct, dk);
        snprintf(name, sizeof(name), "ml-kem-768 decaps reference ct #%u", i);
        check(memcmp(ss, v->ss, 32) == 0, name);
        memset(m, (int)i + 1, 32);
        check(sc_mlkem768_encaps(ct, ss, ek, m) == 0, "ml-kem-768 encaps");
        sc_mlkem768_decaps(ss2, ct, dk);
        snprintf(name, sizeof(name), "ml-kem-768 round trip #%u", i);
        check(memcmp(ss, ss2, 32) == 0, name);
        ct[17] ^= 4;
        sc_mlkem768_decaps(ss2, ct, dk);
        snprintf(name, sizeof(name), "ml-kem-768 implicit rejection #%u", i);
        check(memcmp(ss, ss2, 32) != 0, name);
    }
    ek[0] = 0xff;
    ek[1] |= 0x0f; /* coefficient 4095 >= q */
    check(sc_mlkem768_encaps(ct, ss, ek, m) == -1, "ml-kem-768 rejects non-canonical ek");
}

static sc_wrap_pub_t alice_pub, bob_pub;
static sc_wrap_priv_t alice, bob;

static void test_wrap(void) {
    unsigned char seed[96], rnd[SC_WRAP_RANDOM], key[32], got[32], box[SC_WRAP_BOX];
    static const char purpose[] = "fk:file-0001:epoch-1";
    for (int i = 0; i < 96; i++) seed[i] = (unsigned char)(i * 3 + 1);
    sc_wrap_keypair(&alice_pub, &alice, seed);
    seed[0] ^= 1;
    seed[40] ^= 1;
    sc_wrap_keypair(&bob_pub, &bob, seed);
    for (int i = 0; i < SC_WRAP_RANDOM; i++) rnd[i] = (unsigned char)(i * 5 + 7);
    for (int i = 0; i < 32; i++) key[i] = (unsigned char)(0xa0 + i);

    check(sc_wrap(box, &alice_pub, key, (const unsigned char *)purpose, sizeof(purpose) - 1, rnd) == 0, "wrap");
    check(sc_unwrap(got, box, &alice_pub, &alice, (const unsigned char *)purpose, sizeof(purpose) - 1) == 0 &&
          memcmp(got, key, 32) == 0, "unwrap round trip");
    check(sc_unwrap(got, box, &bob_pub, &bob, (const unsigned char *)purpose, sizeof(purpose) - 1) == -1,
          "unwrap rejects the wrong recipient");
    check(sc_unwrap(got, box, &alice_pub, &alice, (const unsigned char *)"fk:file-0002:epoch-1", 20) == -1,
          "unwrap rejects a different purpose");
    for (int pos = 0; pos < SC_WRAP_BOX; pos += 97) {
        box[pos] ^= 0x20;
        if (sc_unwrap(got, box, &alice_pub, &alice, (const unsigned char *)purpose, sizeof(purpose) - 1) == 0) {
            check(0, "unwrap rejects a tampered box");
        }
        box[pos] ^= 0x20;
    }
    check(1, "unwrap rejects a tampered box (13 positions)");
}

int main(void) {
    test_rfc();
    test_keccak_blake2b();
    test_argon2();
    test_x25519();
    test_mlkem();
    test_wrap();
    test_hashes();
    test_aead(ietf_vecs, N(ietf_vecs), 0);
    test_aead(x_vecs, N(x_vecs), 1);
    test_ed25519();
    test_drbg();
    printf("%d/%d checks passed\n", checks - failures, checks);
    return failures ? 1 : 0;
}
