#include "wrap.h"
#include "aead.h"
#include "ed25519.h"
#include "sha2.h"
#include "sc.h"

static const char LABEL[] = "shusha-wrap-v1";

void sc_wrap_keypair(sc_wrap_pub_t *pub, sc_wrap_priv_t *priv, const uint8_t seed[96]) {
    for (int i = 0; i < 32; i++) priv->x25519[i] = seed[i];
    sc_x25519_base(pub->x25519, priv->x25519);
    sc_mlkem768_keygen(pub->mlkem, priv->mlkem, seed + 32);
}

void sc_wrap_keyid(uint8_t id[16], const sc_wrap_pub_t *pub) {
    uint8_t h[32];
    sc_sha256_t st;
    sc_sha256_init(&st);
    sc_sha256_update(&st, pub->x25519, 32);
    sc_sha256_update(&st, pub->mlkem, SC_MLKEM_EK);
    sc_sha256_final(&st, h);
    for (int i = 0; i < 16; i++) id[i] = h[i];
}

/* kek = HKDF(salt = e_pub || ct, ikm = ss1 || ss2,
 *            info = label || keyid || purpose) */
static int derive(uint8_t kek[32], const uint8_t *e_pub, const uint8_t *mct,
                  const uint8_t ss1[32], const uint8_t ss2[32], const sc_wrap_pub_t *to,
                  const uint8_t *purpose, size_t plen) {
    uint8_t salt[32 + SC_MLKEM_CT], ikm[64], info[sizeof(LABEL) - 1 + 16 + SC_WRAP_PURPOSE_MAX];
    size_t n = 0;
    if (plen > SC_WRAP_PURPOSE_MAX) return -1;
    for (int i = 0; i < 32; i++) salt[i] = e_pub[i];
    for (int i = 0; i < SC_MLKEM_CT; i++) salt[32 + i] = mct[i];
    for (int i = 0; i < 32; i++) {
        ikm[i] = ss1[i];
        ikm[32 + i] = ss2[i];
    }
    for (size_t i = 0; i < sizeof(LABEL) - 1; i++) info[n++] = (uint8_t)LABEL[i];
    sc_wrap_keyid(info + n, to);
    n += 16;
    for (size_t i = 0; i < plen; i++) info[n++] = purpose[i];
    sc_hkdf(salt, sizeof(salt), ikm, sizeof(ikm), info, n, kek, 32);
    sc_wipe(ikm, sizeof(ikm));
    return 0;
}

static int all_zero(const uint8_t *p, size_t n) {
    uint8_t acc = 0;
    for (size_t i = 0; i < n; i++) acc |= p[i];
    return acc == 0;
}

int sc_wrap(uint8_t box[SC_WRAP_BOX], const sc_wrap_pub_t *to, const uint8_t key[32],
            const uint8_t *purpose, size_t plen, const uint8_t rnd[SC_WRAP_RANDOM]) {
    uint8_t *e_pub = box, *mct = box + 32, *nonce = mct + SC_MLKEM_CT, *ct = nonce + 24, *tag = ct + 32;
    uint8_t ss1[32], ss2[32], kek[32];
    int r = -1;

    sc_x25519_base(e_pub, rnd);
    sc_x25519(ss1, rnd, to->x25519);
    if (all_zero(ss1, 32)) goto out; /* low-order recipient key */
    if (sc_mlkem768_encaps(mct, ss2, to->mlkem, rnd + 32) != 0) goto out;
    for (int i = 0; i < 24; i++) nonce[i] = rnd[64 + i];
    if (derive(kek, e_pub, mct, ss1, ss2, to, purpose, plen) != 0) goto out;
    sc_xaead_seal(ct, tag, key, 32, purpose, plen, nonce, kek);
    r = 0;
out:
    sc_wipe(ss1, sizeof(ss1));
    sc_wipe(ss2, sizeof(ss2));
    sc_wipe(kek, sizeof(kek));
    return r;
}

int sc_unwrap(uint8_t key[32], const uint8_t box[SC_WRAP_BOX], const sc_wrap_pub_t *me,
              const sc_wrap_priv_t *priv, const uint8_t *purpose, size_t plen) {
    const uint8_t *e_pub = box, *mct = box + 32, *nonce = mct + SC_MLKEM_CT, *ct = nonce + 24, *tag = ct + 32;
    uint8_t ss1[32], ss2[32], kek[32];
    int r = -1;

    sc_x25519(ss1, priv->x25519, e_pub);
    if (all_zero(ss1, 32)) goto out;
    sc_mlkem768_decaps(ss2, mct, priv->mlkem);
    if (derive(kek, e_pub, mct, ss1, ss2, me, purpose, plen) != 0) goto out;
    r = sc_xaead_open(key, ct, 32, tag, purpose, plen, nonce, kek);
out:
    sc_wipe(ss1, sizeof(ss1));
    sc_wipe(ss2, sizeof(ss2));
    sc_wipe(kek, sizeof(kek));
    return r;
}
