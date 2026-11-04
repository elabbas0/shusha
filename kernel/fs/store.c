/* Shusha's state store.
 *
 * Everything a machine keeps between boots (accounts, homes, the
 * workspace) is the writable part of the VFS tree.  The store saves it
 * as one image (persistfs format v2: data, owners and modes), encrypted
 * with XChaCha20-Poly1305 under a random machine key MK, on a raw GPT
 * partition of its own:
 *
 *   LBA 0      keyslot: MK wrapped two ways (below), salt, which
 *              hardware IDs were present
 *   LBA 8, 9   commit records for slot 0 and slot 1
 *   LBA 64..   slot 0, then slot 1 (each half of the rest)
 *
 * Saves alternate between the slots and write the slot's commit record
 * last, so a crash mid-save leaves the previous save intact; at boot the
 * newest commit whose image decrypts wins.
 *
 * MK is bound to the machine, with no TPM (requirement R10): a random
 * hw_key wraps MK, and hw_key is split 2-of-3 (Shamir over GF(256)) into
 * shares, each wrapped under one hardware ID:
 *
 *   K_i = SHA-256("shusha-hw-v1" || i || salt || id_i)
 *   ids: SMBIOS system UUID (GRUB passes it as shusha.uuid=), the NIC's
 *        MAC address, and the state disk's serial number
 *
 * Any two matching IDs rebuild hw_key, so replacing one part is fine (and
 * the binding then moves to the new part).  Moving the disk to another
 * machine leaves only the disk serial: locked.  A recovery code shown
 * once at install also wraps MK; using it rebinds the disk to the current
 * machine.  Hardware IDs are not secrets (they are readable and often
 * printed on labels), so this stops the disk being read elsewhere by
 * default; it is not a substitute for a password.  See docs/STORAGE.md.
 */
#include "store.h"
#include "persistfs.h"
#include "vfs.h"
#include "diskfmt.h"
#include "install.h"
#include "../drivers/storage/block.h"
#include "../drivers/storage/partition.h"
#include "../drivers/serial/serial.h"
#include "../memory/heap.h"
#include "../crypto/random.h"
#include "../net/net.h"
#include "aead.h"
#include "sha2.h"
#include "sc.h"

#define SECTOR        512U
#define KS_LBA        0U
#define CR_LBA        8U
#define DATA_LBA      64U
#define IO_CHUNK      128U
#define NIDS          3

/* GPT type of the state partition ("Shusha State"). */
static const uint8_t state_type_guid[16] = {
    0x53, 0x48, 0x55, 0x53, 0x48, 0x41, 0x53, 0x54, 0xa7, 0x1c, 0x4e, 0x2b, 0x9d, 0x60, 0x13, 0x77
};

typedef struct __attribute__((packed)) {
    uint8_t nonce[24];
    uint8_t ct[32];
    uint8_t tag[16];
} box_t;

typedef struct __attribute__((packed)) {
    char     magic[8];          /* "SHUSHAKS" */
    uint32_t version;           /* 1 */
    uint8_t  threshold;         /* IDs needed: 2, or 1 if only one was present */
    uint8_t  present;           /* bit i: id i was present when bound */
    uint8_t  pad[2];
    uint8_t  salt[16];
    box_t    share[NIDS];       /* share_i under K_i */
    box_t    hw;                /* MK under hw_key */
    box_t    rec;               /* MK under the recovery key */
    uint8_t  rec_salt[16];
    uint64_t installed;         /* generation counter at install (info) */
    uint32_t crc;
} keyslot_t;

typedef struct __attribute__((packed)) {
    char     magic[8];          /* "SHUSHACR" */
    uint64_t generation;
    uint32_t slot;
    uint32_t pad;
    uint64_t bytes;             /* image length */
    uint8_t  nonce[24];
    uint8_t  tag[16];
    uint32_t crc;
} commit_t;

static int st = STORE_NONE;
static block_device_t *sdev;
static int sdev_index = -1;
static uint64_t part_start, part_sectors, slot_sectors;
static keyslot_t ks;
static uint8_t mk[32];
static uint64_t generation;
static uint64_t saved_bytes;
static uint8_t last_hash[32];
static uint64_t last_changes = (uint64_t)-1;
static int matched_ids, present_ids;
static char ids_text[48];
static char last_error[64];
static char pending_code[32];   /* recovery code from this boot's install, root only */
static char cmdline_copy[512];

/* ---- small helpers -------------------------------------------------------- */

static uint64_t slen(const char *s) {
    uint64_t n = 0;
    while (s && s[n]) n++;
    return n;
}

static void scopy(char *d, const char *s, uint64_t cap) {
    uint64_t i = 0;
    if (!cap) return;
    while (s && s[i] && i + 1 < cap) { d[i] = s[i]; i++; }
    d[i] = 0;
}

static void scat(char *d, const char *s, uint64_t cap) {
    uint64_t n = slen(d);
    if (n < cap) scopy(d + n, s, cap - n);
}

static void sdec(char *d, uint64_t v, uint64_t cap) {
    char t[24];
    int n = 0;
    do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    for (int i = 0; i < n && (uint64_t)i + 1 < cap; i++) d[i] = t[n - 1 - i];
    d[(uint64_t)n < cap ? (uint64_t)n : cap - 1] = 0;
}

static void catdec(char *d, uint64_t v, uint64_t cap) {
    char t[24];
    sdec(t, v, sizeof(t));
    scat(d, t, cap);
}

static void set_error(const char *e) {
    scopy(last_error, e, sizeof(last_error));
    serial_write("store: ");
    serial_write(e);
    serial_write("\n");
}

static uint32_t crc32(const uint8_t *p, uint64_t n) {
    uint32_t c = 0xffffffffU;
    for (uint64_t i = 0; i < n; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xedb88320U & (0U - (c & 1U)));
    }
    return ~c;
}

static int disk_read(uint64_t lba, uint64_t count, void *buf) {
    uint8_t *b = (uint8_t *)buf;
    while (count) {
        uint32_t n = count > IO_CHUNK ? IO_CHUNK : (uint32_t)count;
        if (sdev->read(sdev->context, part_start + lba, n, b) != 0) return -1;
        lba += n;
        count -= n;
        b += (uint64_t)n * SECTOR;
    }
    return 0;
}

static int disk_write(uint64_t lba, uint64_t count, const void *buf) {
    const uint8_t *b = (const uint8_t *)buf;
    while (count) {
        uint32_t n = count > IO_CHUNK ? IO_CHUNK : (uint32_t)count;
        if (sdev->write(sdev->context, part_start + lba, n, b) != 0) return -1;
        lba += n;
        count -= n;
        b += (uint64_t)n * SECTOR;
    }
    return 0;
}

/* ---- GF(256) and the 2-of-3 split ---------------------------------------- */

static uint8_t gf_mul(uint8_t a, uint8_t b) {
    uint8_t r = 0;
    for (int i = 0; i < 8; i++) {
        uint8_t mask = (uint8_t)(0U - (b & 1U));
        r ^= (uint8_t)(a & mask);
        a = (uint8_t)((a << 1) ^ (0x1bU & (uint8_t)(0U - (a >> 7))));
        b >>= 1;
    }
    return r;
}

static uint8_t gf_inv(uint8_t a) {
    /* a^254 = a^-1 in GF(2^8) */
    uint8_t r = 1, x = a;
    for (int e = 254; e; e >>= 1) {
        if (e & 1) r = gf_mul(r, x);
        x = gf_mul(x, x);
    }
    return r;
}

/* f(x) = s + a*x; share i is f(i + 1). */
static void split(const uint8_t s[32], const uint8_t a[32], uint8_t out[NIDS][32]) {
    for (int i = 0; i < NIDS; i++) {
        for (int j = 0; j < 32; j++) out[i][j] = (uint8_t)(s[j] ^ gf_mul(a[j], (uint8_t)(i + 1)));
    }
}

static void combine(int i, const uint8_t yi[32], int k, const uint8_t yk[32], uint8_t s[32]) {
    uint8_t xi = (uint8_t)(i + 1), xk = (uint8_t)(k + 1), inv = gf_inv((uint8_t)(xi ^ xk));
    for (int j = 0; j < 32; j++) {
        uint8_t a = gf_mul((uint8_t)(yi[j] ^ yk[j]), inv);
        s[j] = (uint8_t)(yi[j] ^ gf_mul(a, xi));
    }
}

/* ---- hardware IDs ---------------------------------------------------------- */

static char id_val[NIDS][48];
static int id_len[NIDS];

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void cmdline_value(const char *key, char *out, uint64_t cap) {
    const char *s = cmdline_copy;
    uint64_t kl = slen(key);
    out[0] = 0;
    for (uint64_t i = 0; s[i]; i++) {
        if ((i == 0 || s[i - 1] == ' ') ) {
            uint64_t j = 0;
            while (j < kl && s[i + j] == key[j]) j++;
            if (j == kl) {
                uint64_t n = 0;
                i += kl;
                while (s[i] && s[i] != ' ' && n + 1 < cap) out[n++] = s[i++];
                out[n] = 0;
                return;
            }
        }
    }
}

static void gather_ids(void) {
    char uuid[64];
    int n = 0, nonzero = 0;
    for (int i = 0; i < NIDS; i++) { id_len[i] = 0; id_val[i][0] = 0; }

    /* 0: SMBIOS system UUID, as 32 lowercase hex digits */
    cmdline_value("shusha.uuid=", uuid, sizeof(uuid));
    scopy(id_val[0], "uuid:", sizeof(id_val[0]));
    n = 5;
    for (uint64_t i = 0; uuid[i] && n < 47; i++) {
        int v = hexval(uuid[i]);
        if (v < 0) continue;
        id_val[0][n++] = "0123456789abcdef"[v];
        if (v) nonzero = 1;
    }
    id_val[0][n] = 0;
    if (n == 5 + 32 && nonzero) id_len[0] = n;

    /* 1: the network card's MAC address */
    scopy(id_val[1], "mac:", sizeof(id_val[1]));
    n = 4;
    nonzero = 0;
    for (int i = 0; i < 6; i++) {
        id_val[1][n++] = "0123456789abcdef"[net_state.mac[i] >> 4];
        id_val[1][n++] = "0123456789abcdef"[net_state.mac[i] & 15];
        if (net_state.mac[i]) nonzero = 1;
    }
    id_val[1][n] = 0;
    if (nonzero) id_len[1] = n;

    /* 2: the state disk's serial number */
    scopy(id_val[2], "disk:", sizeof(id_val[2]));
    if (sdev && sdev->serial[0]) {
        scat(id_val[2], sdev->serial, sizeof(id_val[2]));
        id_len[2] = (int)slen(id_val[2]);
    }

    ids_text[0] = 0;
    scat(ids_text, id_len[0] ? "uuid " : "", sizeof(ids_text));
    scat(ids_text, id_len[1] ? "mac " : "", sizeof(ids_text));
    scat(ids_text, id_len[2] ? "disk" : "", sizeof(ids_text));
}

static void id_key(int i, const uint8_t salt[16], uint8_t out[32]) {
    sc_sha256_t h;
    uint8_t idx = (uint8_t)i;
    sc_sha256_init(&h);
    sc_sha256_update(&h, (const uint8_t *)"shusha-hw-v1", 12);
    sc_sha256_update(&h, &idx, 1);
    sc_sha256_update(&h, salt, 16);
    sc_sha256_update(&h, (const uint8_t *)id_val[i], (size_t)id_len[i]);
    sc_sha256_final(&h, out);
}

static void box_seal(box_t *b, const uint8_t key[32], const uint8_t secret[32], const char *purpose) {
    (void)random_get(b->nonce, sizeof(b->nonce));
    sc_xaead_seal(b->ct, b->tag, secret, 32, (const uint8_t *)purpose, slen(purpose), b->nonce, key);
}

static int box_open(const box_t *b, const uint8_t key[32], uint8_t secret[32], const char *purpose) {
    return sc_xaead_open(secret, b->ct, 32, b->tag, (const uint8_t *)purpose, slen(purpose), b->nonce, key);
}

/* ---- recovery code ----------------------------------------------------------- */

static const char code_alpha[33] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";

/* 20 symbols (100 bits) shown as XXXXX-XXXXX-XXXXX-XXXXX. */
static void new_code(char out[32]) {
    uint8_t r[20];
    int n = 0;
    (void)random_get(r, sizeof(r));
    for (int i = 0; i < 20; i++) {
        if (i && i % 5 == 0) out[n++] = '-';
        out[n++] = code_alpha[r[i] & 31];
    }
    out[n] = 0;
    sc_wipe(r, sizeof(r));
}

static void code_key(const char *code, const uint8_t salt[16], uint8_t out[32]) {
    char norm[32];
    int n = 0;
    sc_sha256_t h;
    for (uint64_t i = 0; code[i] && n < 31; i++) {
        char c = code[i];
        if (c >= 'a' && c <= 'z') c = (char)(c - 32);
        if (c == '-' || c == ' ') continue;
        norm[n++] = c;
    }
    sc_sha256_init(&h);
    sc_sha256_update(&h, (const uint8_t *)"shusha-store-recovery-v1", 24);
    sc_sha256_update(&h, salt, 16);
    sc_sha256_update(&h, (const uint8_t *)norm, (size_t)n);
    sc_sha256_final(&h, out);
    sc_wipe(norm, sizeof(norm));
}

/* ---- keyslot ------------------------------------------------------------------- */

static int keyslot_write(void) {
    uint8_t sector[SECTOR];
    for (uint32_t i = 0; i < SECTOR; i++) sector[i] = 0;
    ks.crc = 0;
    ks.crc = crc32((const uint8_t *)&ks, sizeof(ks) - 4);
    for (uint32_t i = 0; i < sizeof(ks); i++) sector[i] = ((const uint8_t *)&ks)[i];
    return disk_write(KS_LBA, 1, sector);
}

/* Bind MK to the hardware as it is now (install, recovery, or after one
 * ID changed). */
static void bind_hardware(int fresh) {
    uint8_t hw_key[32], a[32], shares[NIDS][32], k[32];
    int present = 0;
    (void)random_get(hw_key, sizeof(hw_key));
    (void)random_get(a, sizeof(a));
    (void)random_get(ks.salt, sizeof(ks.salt));
    ks.present = 0;
    for (int i = 0; i < NIDS; i++) {
        if (id_len[i]) { ks.present |= (uint8_t)(1U << i); present++; }
    }
    ks.threshold = present >= 2 ? 2 : 1;
    if (ks.threshold == 2) {
        split(hw_key, a, shares);
    } else {
        for (int i = 0; i < NIDS; i++) for (int j = 0; j < 32; j++) shares[i][j] = hw_key[j];
    }
    for (int i = 0; i < NIDS; i++) {
        if (ks.present & (1U << i)) {
            id_key(i, ks.salt, k);
            box_seal(&ks.share[i], k, shares[i], "shusha-share");
        } else {
            for (uint32_t j = 0; j < sizeof(box_t); j++) ((uint8_t *)&ks.share[i])[j] = 0;
        }
    }
    box_seal(&ks.hw, hw_key, mk, "shusha-mk-hw");
    present_ids = present;
    if (fresh) matched_ids = present;   /* else: keep what matched at boot, for the status */
    sc_wipe(hw_key, sizeof(hw_key));
    sc_wipe(a, sizeof(a));
    sc_wipe(shares, sizeof(shares));
    sc_wipe(k, sizeof(k));
}

/* Returns 0 and fills mk when enough IDs match. */
static int unlock_hardware(void) {
    uint8_t got[NIDS][32], k[32], hw_key[32];
    int ok[NIDS], first = -1, second = -1, rc = -1;
    matched_ids = 0;
    present_ids = 0;
    for (int i = 0; i < NIDS; i++) {
        ok[i] = 0;
        if (!(ks.present & (1U << i))) continue;
        present_ids++;
        if (!id_len[i]) continue;
        id_key(i, ks.salt, k);
        if (box_open(&ks.share[i], k, got[i], "shusha-share") == 0) {
            ok[i] = 1;
            matched_ids++;
            if (first < 0) first = i;
            else if (second < 0) second = i;
        }
    }
    if (ks.threshold == 1 && first >= 0) {
        for (int j = 0; j < 32; j++) hw_key[j] = got[first][j];
        rc = 0;
    } else if (ks.threshold == 2 && second >= 0) {
        combine(first, got[first], second, got[second], hw_key);
        rc = 0;
    }
    if (rc == 0 && box_open(&ks.hw, hw_key, mk, "shusha-mk-hw") != 0) rc = -1;
    (void)ok;
    sc_wipe(got, sizeof(got));
    sc_wipe(k, sizeof(k));
    sc_wipe(hw_key, sizeof(hw_key));
    return rc;
}

/* ---- images ---------------------------------------------------------------------- */

static void commit_ad(const commit_t *c, uint8_t ad[48]) {
    for (int i = 0; i < 8; i++) ad[i] = (uint8_t)c->magic[i];
    sc_store64_le(ad + 8, c->generation);
    sc_store64_le(ad + 16, c->slot);
    sc_store64_le(ad + 24, c->bytes);
    sc_store64_le(ad + 32, part_sectors);
    sc_store64_le(ad + 40, 0x5348555348410001ULL);
}

static int read_commit(int slot, commit_t *c) {
    uint8_t sector[SECTOR];
    if (disk_read(CR_LBA + (uint64_t)slot, 1, sector) != 0) return -1;
    for (uint32_t i = 0; i < sizeof(*c); i++) ((uint8_t *)c)[i] = sector[i];
    if (c->magic[0] != 'S' || c->magic[7] != 'R' || c->slot != (uint32_t)slot) return -1;
    if (crc32((const uint8_t *)c, sizeof(*c) - 4) != c->crc) return -1;
    if (c->bytes == 0 || (c->bytes + SECTOR - 1) / SECTOR > slot_sectors) return -1;
    return 0;
}

/* Decrypt and import a slot's image; 0 on success. */
static int load_slot(const commit_t *c) {
    uint64_t sectors = (c->bytes + SECTOR - 1) / SECTOR;
    uint8_t *buf = (uint8_t *)kmalloc((size_t)(sectors * SECTOR));
    uint8_t ad[48];
    uint64_t loaded = 0;
    if (!buf) return -1;
    if (disk_read(DATA_LBA + (uint64_t)c->slot * slot_sectors, sectors, buf) != 0) {
        kfree(buf);
        return -1;
    }
    commit_ad(c, ad);
    if (sc_xaead_open(buf, buf, (size_t)c->bytes, c->tag, ad, sizeof(ad), c->nonce, mk) != 0) {
        kfree(buf);
        return -2;
    }
    sc_sha256(buf, (size_t)c->bytes, last_hash);
    if (persistfs_import_image((const char *)buf, c->bytes, &loaded) != 0) {
        sc_wipe(buf, (size_t)c->bytes);
        kfree(buf);
        return -3;
    }
    sc_wipe(buf, (size_t)c->bytes);
    kfree(buf);
    generation = c->generation;
    saved_bytes = c->bytes;
    serial_write("store: loaded generation ");
    {
        char t[24];
        sdec(t, generation, sizeof(t));
        serial_write(t);
    }
    serial_write("\n");
    return 0;
}

static int load_newest(void) {
    commit_t c[2];
    int have[2];
    have[0] = read_commit(0, &c[0]) == 0;
    have[1] = read_commit(1, &c[1]) == 0;
    if (!have[0] && !have[1]) {
        generation = 0;   /* installed, never saved */
        return 0;
    }
    if (have[0] && have[1]) {
        int newer = c[1].generation > c[0].generation ? 1 : 0;
        if (load_slot(&c[newer]) == 0) return 0;
        set_error("newest save unreadable, using the previous one");
        return load_slot(&c[1 - newer]) == 0 ? 0 : -1;
    }
    return load_slot(&c[have[1] ? 1 : 0]) == 0 ? 0 : -1;
}

int store_sync(void) {
    char *image = 0;
    uint64_t size = 0, entries = 0, bytes, sectors;
    uint8_t hash[32], sector[SECTOR], ad[48];
    commit_t c;

    if (st != STORE_UNLOCKED) return 0;
    if (vfs_changes() == last_changes) return 0;
    if (persistfs_export_image(&image, &size, &entries) != 0) {
        set_error("could not pack the file tree");
        return -1;
    }
    bytes = ((const uint32_t *)image)[4];   /* header.bytes_used */
    if (bytes < 24 || bytes > size) bytes = size;
    sc_sha256((const uint8_t *)image, (size_t)bytes, hash);
    if (sc_equal(hash, last_hash, 32)) {
        last_changes = vfs_changes();
        kfree(image);
        return 0;
    }
    sectors = (bytes + SECTOR - 1) / SECTOR;
    if (sectors > slot_sectors) {
        set_error("the saved state is larger than the disk's slot");
        kfree(image);
        return -1;
    }

    for (uint32_t i = 0; i < sizeof(c); i++) ((uint8_t *)&c)[i] = 0;
    c.magic[0] = 'S'; c.magic[1] = 'H'; c.magic[2] = 'U'; c.magic[3] = 'S';
    c.magic[4] = 'H'; c.magic[5] = 'A'; c.magic[6] = 'C'; c.magic[7] = 'R';
    c.generation = generation + 1;
    c.slot = (uint32_t)(c.generation & 1U);
    c.bytes = bytes;
    (void)random_get(c.nonce, sizeof(c.nonce));
    commit_ad(&c, ad);
    /* encrypt in place; the buffer is whole sectors (export pads it) */
    sc_xaead_seal((uint8_t *)image, c.tag, (const uint8_t *)image, (size_t)bytes, ad, sizeof(ad), c.nonce, mk);
    for (uint64_t i = bytes; i < sectors * SECTOR && i < size; i++) image[i] = 0;
    if (disk_write(DATA_LBA + (uint64_t)c.slot * slot_sectors, sectors, image) != 0) {
        set_error("disk write failed");
        kfree(image);
        return -1;
    }
    kfree(image);
    c.crc = crc32((const uint8_t *)&c, sizeof(c) - 4);
    for (uint32_t i = 0; i < SECTOR; i++) sector[i] = 0;
    for (uint32_t i = 0; i < sizeof(c); i++) sector[i] = ((const uint8_t *)&c)[i];
    if (disk_write(CR_LBA + c.slot, 1, sector) != 0) {
        set_error("commit write failed");
        return -1;
    }
    generation = c.generation;
    saved_bytes = bytes;
    for (int i = 0; i < 32; i++) last_hash[i] = hash[i];
    last_changes = vfs_changes();
    return 0;
}

/* ---- finding and opening the store --------------------------------------------- */

static int guid_is_state(const uint8_t *g) {
    for (int i = 0; i < 16; i++) if (g[i] != state_type_guid[i]) return 0;
    return 1;
}

static int use_partition(const partition_info_t *p) {
    sdev = p->device;
    sdev_index = -1;
    for (uint32_t i = 0; i < block_count(); i++) {
        if (block_get(i) == sdev) sdev_index = (int)i;
    }
    part_start = p->start_lba;
    part_sectors = p->sector_count;
    if (part_sectors < DATA_LBA + 2048U) return -1;
    slot_sectors = (part_sectors - DATA_LBA) / 2U;
    return 0;
}

static const partition_info_t *find_state_partition(void) {
    for (uint32_t i = 0; i < partition_count(); i++) {
        const partition_info_t *p = partition_get(i);
        if (p && p->kind == PARTITION_KIND_GPT && guid_is_state(p->gpt_type_guid)) return p;
    }
    return 0;
}

static int read_keyslot(void) {
    uint8_t sector[SECTOR];
    if (disk_read(KS_LBA, 1, sector) != 0) return -1;
    for (uint32_t i = 0; i < sizeof(ks); i++) ((uint8_t *)&ks)[i] = sector[i];
    if (ks.magic[0] != 'S' || ks.magic[6] != 'K' || ks.magic[7] != 'S' || ks.version != 1) return -1;
    if (crc32((const uint8_t *)&ks, sizeof(ks) - 4) != ks.crc) return -1;
    return 0;
}

static void open_unlocked(void) {
    st = STORE_UNLOCKED;
    last_changes = (uint64_t)-1;
    vfs_set_sync_hook(store_sync);
}

void store_init(const char *cmdline, int live) {
    const partition_info_t *p;
    scopy(cmdline_copy, cmdline ? cmdline : "", sizeof(cmdline_copy));
    st = STORE_NONE;
    if (live) {
        /* The live session never opens an installed disk by itself. */
        gather_ids();
        return;
    }
    p = find_state_partition();
    if (!p || use_partition(p) != 0) {
        gather_ids();
        return;
    }
    gather_ids();
    if (read_keyslot() != 0) {
        st = STORE_DAMAGED;
        set_error("the state partition has no valid keyslot");
        return;
    }
    if (unlock_hardware() != 0) {
        st = STORE_LOCKED;
        set_error("this disk is bound to other hardware; the recovery code unlocks it");
        return;
    }
    if (load_newest() != 0) {
        st = STORE_DAMAGED;
        set_error("the saved state could not be decrypted");
        return;
    }
    if (matched_ids < present_ids ||
        (id_len[0] != 0) + (id_len[1] != 0) + (id_len[2] != 0) > matched_ids) {
        /* One ID changed (or a new one appeared): move the binding to the
         * hardware as it is now, so a second change later still works. */
        int was = present_ids;
        bind_hardware(0);
        present_ids = was;   /* the status reports this boot's match */
        if (keyslot_write() == 0) set_error("rebound to this machine's current hardware");
    }
    open_unlocked();
}

/* ---- install --------------------------------------------------------------------- */

static void build_grub_cfg(char *out, uint64_t cap) {
    /* Carry the current boot flags over, except live mode and the UUID. */
    char flags[256];
    flags[0] = 0;
    for (uint64_t i = 0; cmdline_copy[i];) {
        char word[96];
        uint64_t n = 0;
        while (cmdline_copy[i] == ' ') i++;
        while (cmdline_copy[i] && cmdline_copy[i] != ' ' && n + 1 < sizeof(word)) word[n++] = cmdline_copy[i++];
        while (cmdline_copy[i] && cmdline_copy[i] != ' ') i++;
        word[n] = 0;
        if (!n) continue;
        if (word[0] == 'i' && word[1] == 'c' && word[2] == 'd' && word[3] == 'a' && word[4] == '.' && word[5] == 'l') continue;
        if (n >= 12 && word[0] == 's' && word[7] == 'u' && word[8] == 'u' && word[11] == '=') continue;
        scat(flags, " ", sizeof(flags));
        scat(flags, word, sizeof(flags));
    }
    out[0] = 0;
    scat(out,
         "set timeout=0\n"
         "set default=0\n"
         "set gfxpayload=keep\n"
         "insmod part_gpt\n"
         "insmod fat\n"
         "insmod all_video\n"
         "insmod gfxterm\n"
         "insmod smbios\n"
         "set gfxmode=auto\n"
         "smbios --type 1 --get-string 4 --set shusha_vendor\n"
         "if [ \"$shusha_vendor\" = \"QEMU\" ]; then\n"
         "    set gfxmode=1280x800,1024x768,auto\n"
         "fi\n"
         "terminal_output gfxterm\n"
         "smbios --type 1 --get-uuid 8 --set shusha_uuid\n"
         "search --file --set=root /BOOT/KERNEL.BIN\n"
         "menuentry \"Shusha\" {\n"
         "    multiboot2 /BOOT/KERNEL.BIN shusha.uuid=$shusha_uuid", cap);
    scat(out, flags, cap);
    scat(out, "\n    set gfxpayload=keep\n    boot\n}\n", cap);
}

static long store_install(uint32_t device_index) {
    block_device_t *dev = block_get(device_index);
    const partition_info_t *efi = 0, *state = 0;
    uint8_t rk[32];
    char cfg[1536];
    uint32_t salt = 0;
    int rc;

    if (!dev) { set_error("no such disk"); return -1; }
    if (st == STORE_UNLOCKED && dev == sdev) { set_error("that disk is the one in use"); return -1; }
    if (!system_install_assets_present()) { set_error("this kernel cannot install (no boot files)"); return -1; }
    (void)random_get(&salt, sizeof(salt));
    rc = diskfmt_layout_shusha(device_index, state_type_guid, salt);
    if (rc != 0) { set_error("could not partition the disk (too small?)"); return -1; }
    for (uint32_t i = 0; i < partition_count(); i++) {
        const partition_info_t *p = partition_get(i);
        if (!p || p->device != dev || p->kind != PARTITION_KIND_GPT) continue;
        if (p->role == PARTITION_ROLE_EFI) efi = p;
        else if (guid_is_state(p->gpt_type_guid)) state = p;
    }
    if (!efi || !state) { set_error("new partitions not found"); return -1; }
    build_grub_cfg(cfg, sizeof(cfg));
    if (system_install_boot_files(efi, cfg, slen(cfg)) != 0) { set_error("could not write the boot files"); return -1; }

    /* new store on the state partition */
    if (use_partition(state) != 0) { set_error("state partition too small"); return -1; }
    gather_ids();
    for (uint32_t i = 0; i < sizeof(ks); i++) ((uint8_t *)&ks)[i] = 0;
    ks.magic[0] = 'S'; ks.magic[1] = 'H'; ks.magic[2] = 'U'; ks.magic[3] = 'S';
    ks.magic[4] = 'H'; ks.magic[5] = 'A'; ks.magic[6] = 'K'; ks.magic[7] = 'S';
    ks.version = 1;
    (void)random_get(mk, sizeof(mk));
    bind_hardware(1);
    new_code(pending_code);
    (void)random_get(ks.rec_salt, sizeof(ks.rec_salt));
    code_key(pending_code, ks.rec_salt, rk);
    box_seal(&ks.rec, rk, mk, "shusha-mk-recovery");
    sc_wipe(rk, sizeof(rk));
    {
        uint8_t zero[SECTOR * 2];
        for (uint32_t i = 0; i < sizeof(zero); i++) zero[i] = 0;
        if (disk_write(CR_LBA, 2, zero) != 0) { set_error("disk write failed"); return -1; }
    }
    if (keyslot_write() != 0) { set_error("disk write failed"); return -1; }
    generation = 0;
    for (int i = 0; i < 32; i++) last_hash[i] = 0;
    open_unlocked();
    if (store_sync() != 0) return -1;
    set_error("installed");
    return 0;
}

static long store_unlock(const char *code) {
    uint8_t rk[32];
    if (st != STORE_LOCKED) { set_error("the store is not locked"); return -1; }
    code_key(code, ks.rec_salt, rk);
    if (box_open(&ks.rec, rk, mk, "shusha-mk-recovery") != 0) {
        sc_wipe(rk, sizeof(rk));
        set_error("wrong recovery code");
        return -1;
    }
    sc_wipe(rk, sizeof(rk));
    if (load_newest() != 0) {
        st = STORE_DAMAGED;
        set_error("the saved state could not be decrypted");
        return -1;
    }
    bind_hardware(1);
    if (keyslot_write() != 0) { set_error("disk write failed"); return -1; }
    open_unlocked();
    set_error("unlocked with the recovery code and rebound to this machine");
    return 0;
}

/* ---- /dev/store ------------------------------------------------------------------- */

int store_state(void) { return st; }

int store_device(void) { return st == STORE_UNLOCKED || st == STORE_LOCKED ? sdev_index : -1; }

static const char *state_name(void) {
    switch (st) {
    case STORE_UNLOCKED: return "unlocked";
    case STORE_LOCKED: return "locked";
    case STORE_DAMAGED: return "damaged";
    default: return "none";
    }
}

long store_status_text(char *buf, uint64_t cap, int root) {
    char *o = buf;
    if (!cap) return 0;
    o[0] = 0;
    scat(o, "state ", cap); scat(o, state_name(), cap); scat(o, "\n", cap);
    if (st != STORE_NONE && sdev) {
        scat(o, "disk ", cap); scat(o, sdev->name ? sdev->name : "disk", cap);
        scat(o, " ", cap); catdec(o, part_sectors / 2048U, cap); scat(o, " MiB\n", cap);
        scat(o, "generation ", cap); catdec(o, generation, cap); scat(o, "\n", cap);
        scat(o, "saved ", cap); catdec(o, saved_bytes, cap); scat(o, " bytes\n", cap);
        scat(o, "hardware ", cap); catdec(o, (uint64_t)matched_ids, cap); scat(o, "/", cap);
        catdec(o, (uint64_t)present_ids, cap); scat(o, " ids match, ", cap);
        catdec(o, ks.threshold, cap); scat(o, " needed\n", cap);
    }
    scat(o, "ids ", cap); scat(o, ids_text[0] ? ids_text : "none", cap); scat(o, "\n", cap);
    if (root) {
        for (uint32_t i = 0; i < block_count(); i++) {
            block_device_t *d = block_get(i);
            if (!d) continue;
            scat(o, "device ", cap); catdec(o, i, cap); scat(o, " ", cap);
            scat(o, d->name ? d->name : "disk", cap); scat(o, " ", cap);
            catdec(o, d->sector_count / 2048U, cap); scat(o, " MiB", cap);
            if ((st == STORE_UNLOCKED || st == STORE_LOCKED) && d == sdev) scat(o, " in-use", cap);
            scat(o, "\n", cap);
        }
        if (pending_code[0]) { scat(o, "recovery ", cap); scat(o, pending_code, cap); scat(o, "\n", cap); }
        scat(o, "can-install ", cap); scat(o, system_install_assets_present() ? "yes" : "no", cap); scat(o, "\n", cap);
    }
    if (last_error[0]) { scat(o, "note ", cap); scat(o, last_error, cap); scat(o, "\n", cap); }
    return (long)slen(o);
}

static int word_is(const char *s, const char *w) {
    uint64_t n = slen(w);
    for (uint64_t i = 0; i < n; i++) if (s[i] != w[i]) return 0;
    return s[n] == 0 || s[n] == ' ';
}

long store_command(const char *cmd, int root) {
    if (!root || !cmd) return -1;
    while (*cmd == ' ') cmd++;
    if (word_is(cmd, "sync")) return store_sync() == 0 ? 0 : -1;
    if (word_is(cmd, "forget")) { sc_wipe(pending_code, sizeof(pending_code)); return 0; }
    if (word_is(cmd, "install")) {
        uint64_t d = 0;
        const char *p = cmd + 7;
        while (*p == ' ') p++;
        if (*p < '0' || *p > '9') return -1;
        while (*p >= '0' && *p <= '9') d = d * 10 + (uint64_t)(*p++ - '0');
        return store_install((uint32_t)d);
    }
    if (word_is(cmd, "unlock")) {
        const char *p = cmd + 6;
        while (*p == ' ') p++;
        return store_unlock(p);
    }
    return -1;
}
