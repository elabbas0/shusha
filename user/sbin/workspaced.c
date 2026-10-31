/* workspaced: the Shusha Workspace service (docs/WORKSPACE.md).
 *
 * One per machine, started by init.  It owns this device's identity
 * (Ed25519 signing key + X25519/ML-KEM wrap key), the workspace state,
 * and the encrypted file replicas, all under /var/ws (root, 0700).  It
 * talks to other machines only through authenticated secure channels
 * (chan.c) and to local users through the `ws` command (wsproto.h).
 *
 * Model (v1, local network):
 *  - The creator is rank 0 (admin).  Joiners get the next rank in join
 *    order.  The admin signs a hash-chained membership log; every member
 *    re-verifies the whole chain against the genesis admin key.
 *  - Rank keys: RK_r is random; it is wrapped (hybrid X25519+ML-KEM) to
 *    the member of rank r and sealed under RK_(r-1) in the log, so a
 *    member can unlock every rank below its own and none above.
 *  - A shared file gets a random file key FK, wrapped to its owner and
 *    sealed under RK_owner (which only the owner and higher ranks can
 *    derive), plus one box per explicit grant.  The object is signed by the owner (or the higher
 *    rank who granted) and replicated to every member, so members keep
 *    working while the admin is offline.
 *
 * Separation from personal files: the workspace belongs to the local
 * user who created or joined it (other users of this machine are
 * refused), and nothing enters it unless that user adds it - the `ws`
 * client reads the file with the user's own permissions and hands over
 * the contents, so the daemon never opens a personal path itself.
 *
 * Live state for the desktop: members are pinged every 2 s (presence),
 * notable events are kept in a short list, and a summary is written to
 * /var/ws/status (readable only by the workspace's user) for the top bar.
 */
#include "lib.h"
#include "net.h"
#include "chan.h"
#include "wsproto.h"
#include "aead.h"
#include "ed25519.h"
#include "sha2.h"
#include "wrap.h"
#include "sc.h"

#define MAXM        16
#define MAXLOG      64
#define MAXINV      8
#define MAXREQ      16
#define ENTRY_BODY  200
#define ENTRY_SIZE  (4 + 1 + ENTRY_BODY + 32 + 64)
#define MAX_CONTENT 8192
#define MAX_OBJECT  (MAX_CONTENT + 8 * (2 + SC_WRAP_BOX) + 512)
#define CHUNK       1200

enum { E_GENESIS = 1, E_JOIN = 2 };
enum {
    M_JOIN_REQ = 1, M_JOIN_OK, M_JOIN_FAIL, M_LOG_GET, M_LOG_ENTRY, M_LOG_END,
    M_WRAP_GET, M_WRAP, M_NOTIFY, M_FILE_CHUNK, M_FILE_LIST_GET, M_FILE_LIST,
    M_FILE_GET, M_ACCESS_REQ, M_ACK, M_PING, M_PONG
};

#define BEAT_TICKS    200 /* presence ping interval */
#define OFFLINE_TICKS 700 /* silent this long = offline */
#define MAXEV         12
enum { BOX_MEMBER = 1, BOX_RANK = 2 };

typedef struct {
    int used;
    uint8_t rank;
    char name[24];
    uint8_t sign_pub[32];
    uint8_t wrap_hash[32];
    uint8_t chain[72];        /* RK_rank sealed under RK_(rank-1): nonce24 | ct32 | tag16 */
    uint8_t ip[4];
    int have_wrap;
    sc_wrap_pub_t wrap;
    /* runtime only (reset at start) */
    uint64_t last_seen;
    int online, need_sync;
} member_t;

enum { INV_FREE = 0, INV_OPEN, INV_USED };

typedef struct {
    uint8_t secret[16];
    int state;
} invite_t;

typedef struct {
    uint8_t file_id[16];
    uint8_t rank;
} access_req_t;

typedef struct {
    uint32_t magic;
    int active;
    uint32_t owner_uid;       /* the local user this workspace belongs to */
    uint8_t ws_id[16];
    char ws_name[32];
    uint8_t admin_pub[32];
    int my_rank;
    uint8_t my_rk[32];
    member_t m[MAXM];
    uint32_t log_len;
    uint8_t log[MAXLOG][ENTRY_SIZE];
    invite_t inv[MAXINV];
    access_req_t req[MAXREQ];
    int nreq;
} state_t;

#define STATE_MAGIC 0x31535753u /* "SWS1" */

static state_t st;
static uint8_t sign_seed[32], sign_pub[32];
static sc_wrap_pub_t wrap_pub;
static sc_wrap_priv_t wrap_priv;
static uint8_t my_ip[4];

/* Command output for the current `ws` request. */
static char out[MAX_CONTENT + 2048];
static uint64_t out_len;

static void say(const char *s) {
    while (*s && out_len + 1 < sizeof(out)) out[out_len++] = *s++;
    out[out_len] = '\0';
}

static void say_bytes(const uint8_t *p, uint64_t n) {
    while (n-- && out_len + 1 < sizeof(out)) out[out_len++] = (char)*p++;
    out[out_len] = '\0';
}

static void say_u(uint64_t v) {
    char b[24];
    say(fmt_u64(b, v, 10));
}

static void say_hex(const uint8_t *p, int n) {
    static const char hx[] = "0123456789abcdef";
    char b[3] = { 0 };
    for (int i = 0; i < n; i++) {
        b[0] = hx[p[i] >> 4];
        b[1] = hx[p[i] & 15];
        say(b);
    }
}

static void log_line(const char *s) {
    print("workspaced: ");
    print(s);
    print("\n");
}

/* ---- events ------------------------------------------------------------ */

typedef struct {
    uint64_t tick;
    char text[80];
} event_t;

static event_t evs[MAXEV];
static int nev;
static char busy_text[64];
static void write_status(void);

static void event(const char *a, const char *b, const char *c) {
    event_t e;
    e.tick = sh_ticks();
    str_copy(e.text, a, sizeof(e.text));
    if (b) str_cat(e.text, b, sizeof(e.text));
    if (c) str_cat(e.text, c, sizeof(e.text));
    if (nev == MAXEV) {
        for (int i = 1; i < MAXEV; i++) evs[i - 1] = evs[i];
        nev--;
    }
    evs[nev++] = e;
    log_line(e.text);
    write_status();
}

/* Shown in the top bar while a slow network operation runs. */
static void set_busy(const char *a, const char *b) {
    str_copy(busy_text, a ? a : "", sizeof(busy_text));
    if (b) str_cat(busy_text, b, sizeof(busy_text));
    write_status();
}

/* ---- persistence ------------------------------------------------------- */

static void save_state(void) {
    (void)sh_write_file(WS_DIR "/state", &st, sizeof(st));
    (void)sh_chmod(WS_DIR "/state", 0600);
}

static void load_or_create_identity(void) {
    uint8_t id[32 + 96];
    if (sh_read_at(WS_DIR "/id", 0, (char *)id, sizeof(id)) != (long)sizeof(id)) {
        rand_bytes(id, sizeof(id));
        (void)sh_write_file(WS_DIR "/id", id, sizeof(id));
        (void)sh_chmod(WS_DIR "/id", 0600);
    }
    mem_copy(sign_seed, id, 32);
    sc_ed25519_public_key(sign_pub, sign_seed);
    sc_wrap_keypair(&wrap_pub, &wrap_priv, id + 32);
    mem_wipe(id, sizeof(id));
}

static void load_state(void) {
    if (sh_read_at(WS_DIR "/state", 0, (char *)&st, sizeof(st)) != (long)sizeof(st) || st.magic != STATE_MAGIC) {
        mem_set(&st, 0, sizeof(st));
        st.magic = STATE_MAGIC;
    }
}

/* ---- membership log ---------------------------------------------------- */

static void entry_hash(uint8_t out32[32], const uint8_t *e) {
    sc_sha256(e, 4 + 1 + ENTRY_BODY + 32, out32);
}

static member_t *member_by_rank(int rank) {
    for (int i = 0; i < MAXM; i++) {
        if (st.m[i].used && st.m[i].rank == rank) return &st.m[i];
    }
    return 0;
}

static member_t *member_by_pub(const uint8_t pub[32]) {
    for (int i = 0; i < MAXM; i++) {
        if (st.m[i].used && mem_eq_ct(st.m[i].sign_pub, pub, 32)) return &st.m[i];
    }
    return 0;
}

static int member_count(void) {
    int n = 0;
    for (int i = 0; i < MAXM; i++) n += st.m[i].used;
    return n;
}

/* Rebuild the member table from the log, verifying every signature and
 * hash link against the genesis admin key.  Returns 0 if valid. */
static int replay_log(void) {
    uint8_t prev[32] = { 0 }, h[32], admin[32];
    member_t keep[MAXM];
    mem_copy(keep, st.m, sizeof(keep));
    mem_set(st.m, 0, sizeof(st.m));
    for (uint32_t i = 0; i < st.log_len; i++) {
        const uint8_t *e = st.log[i];
        const uint8_t *body = e + 5;
        if (sc_load32_le(e) != i || !mem_eq_ct(e + 5 + ENTRY_BODY, prev, 32)) goto bad;
        entry_hash(h, e);
        if (i == 0) {
            if (e[4] != E_GENESIS) goto bad;
            mem_copy(admin, body + 48, 32);
        } else if (e[4] != E_JOIN) {
            goto bad;
        }
        if (sc_ed25519_verify(e + 5 + ENTRY_BODY + 32, h, 32, admin) != 0) goto bad;
        if (i == 0) {
            mem_copy(st.ws_id, body, 16);
            mem_copy(st.ws_name, body + 16, 32);
            st.ws_name[31] = '\0';
            mem_copy(st.admin_pub, admin, 32);
            st.m[0].used = 1;
            st.m[0].rank = 0;
            mem_copy(st.m[0].name, body + 112, 24);
            st.m[0].name[23] = '\0';
            mem_copy(st.m[0].sign_pub, admin, 32);
            mem_copy(st.m[0].wrap_hash, body + 80, 32);
            mem_copy(st.m[0].ip, body + 136, 4);
        } else {
            member_t *m = 0;
            int r = body[0];
            if (r >= MAXM || member_by_rank(r)) goto bad;
            for (int k = 0; k < MAXM; k++) {
                if (!st.m[k].used) {
                    m = &st.m[k];
                    break;
                }
            }
            if (!m) goto bad;
            m->used = 1;
            m->rank = (uint8_t)r;
            mem_copy(m->name, body + 1, 24);
            m->name[23] = '\0';
            mem_copy(m->sign_pub, body + 25, 32);
            mem_copy(m->wrap_hash, body + 57, 32);
            mem_copy(m->chain, body + 89, 72);
            mem_copy(m->ip, body + 161, 4);
        }
        mem_copy(prev, h, 32);
    }
    /* Keep wrap keys we already fetched (they are pinned by wrap_hash). */
    for (int i = 0; i < MAXM; i++) {
        member_t *m = st.m[i].used ? &st.m[i] : 0;
        if (!m) continue;
        for (int k = 0; k < MAXM; k++) {
            if (!keep[k].used || !mem_eq_ct(keep[k].sign_pub, m->sign_pub, 32)) continue;
            if (keep[k].have_wrap) {
                m->have_wrap = 1;
                m->wrap = keep[k].wrap;
            }
            m->last_seen = keep[k].last_seen;
            m->online = keep[k].online;
            m->need_sync = keep[k].need_sync;
        }
        if (mem_eq_ct(m->sign_pub, sign_pub, 32)) {
            m->have_wrap = 1;
            m->wrap = wrap_pub;
        }
    }
    return 0;
bad:
    mem_copy(st.m, keep, sizeof(keep));
    return -1;
}

static void wrap_hash_of(uint8_t out32[32], const sc_wrap_pub_t *w) {
    sc_sha256((const uint8_t *)w, sizeof(*w), out32);
}

/* Admin: sign and append an entry. */
static int append_entry(uint8_t type, const uint8_t body[ENTRY_BODY]) {
    uint8_t *e, h[32];
    if (st.log_len >= MAXLOG) return -1;
    e = st.log[st.log_len];
    sc_store32_le(e, st.log_len);
    e[4] = type;
    mem_copy(e + 5, body, ENTRY_BODY);
    if (st.log_len) entry_hash(e + 5 + ENTRY_BODY, st.log[st.log_len - 1]);
    else mem_set(e + 5 + ENTRY_BODY, 0, 32);
    entry_hash(h, e);
    sc_ed25519_sign(e + 5 + ENTRY_BODY + 32, h, 32, sign_seed, sign_pub);
    st.log_len++;
    return replay_log();
}

/* ---- rank keys --------------------------------------------------------- */

static void seal_sym(uint8_t out72[72], const uint8_t key[32], const uint8_t secret[32], const char *purpose) {
    rand_bytes(out72, 24);
    sc_xaead_seal(out72 + 24, out72 + 56, secret, 32, (const uint8_t *)purpose, str_len(purpose), out72, key);
}

static int open_sym(uint8_t secret[32], const uint8_t in72[72], const uint8_t key[32], const char *purpose) {
    return sc_xaead_open(secret, in72 + 24, 32, in72 + 56, (const uint8_t *)purpose, str_len(purpose), in72, key);
}

static void rk_purpose(char *buf, int rank) {
    char n[8];
    str_copy(buf, "rk:", 48);
    str_cat(buf, fmt_u64(n, (uint64_t)rank, 10), 48);
}

/* Derive RK_target from our own rank key by walking down the chain. */
static int rank_key(uint8_t out32[32], int target) {
    uint8_t k[32];
    char purpose[48];
    if (!st.active || target < st.my_rank) return -1;
    mem_copy(k, st.my_rk, 32);
    for (int r = st.my_rank + 1; r <= target; r++) {
        member_t *m = member_by_rank(r);
        uint8_t next[32];
        rk_purpose(purpose, r);
        if (!m || open_sym(next, m->chain, k, purpose) != 0) {
            mem_wipe(k, 32);
            return -1;
        }
        mem_copy(k, next, 32);
        mem_wipe(next, 32);
    }
    mem_copy(out32, k, 32);
    mem_wipe(k, 32);
    return 0;
}

/* ---- RPC over channels ------------------------------------------------- */

static void handle_peer(chan_msg_t *m);
static int decrypt_name(char name[65], const uint8_t *o, const uint8_t fk[32]);

static chan_msg_t rx;

/* Send req and wait for a reply of type `want` on the same channel,
 * serving other peers meanwhile. */
static int rpc(int ch, const uint8_t *req, uint64_t len, uint8_t want, chan_msg_t *resp, uint64_t ticks) {
    for (int attempt = 0; attempt < 3; attempt++) {
        uint64_t end = sh_ticks() + ticks;
        if (chan_send(ch, req, len) != 0) return -1;
        while (sh_ticks() < end) {
            if (chan_poll(resp)) {
                if (resp->chan == ch && (resp->data[0] == want || resp->data[0] == M_JOIN_FAIL ||
                                         resp->data[0] == M_LOG_END)) {
                    return resp->data[0] == want ? 0 : -1;
                }
                handle_peer(resp);
                continue;
            }
            sh_sleep(1);
        }
    }
    return -1;
}

static int is_me(const member_t *m) {
    return mem_eq_ct(m->sign_pub, sign_pub, 32);
}

/* A channel to a member: the open one if there is one; a fresh handshake
 * only if the member is believed online, so offline machines never
 * stall the daemon. */
static int connect_member(const member_t *m) {
    int ch;
    const uint8_t *peer;
    if (is_me(m) || mem_eq_ct(m->ip, my_ip, 4)) return -1;
    ch = chan_find(m->sign_pub);
    if (ch >= 0) return ch;
    if (!m->online) return -1;
    ch = chan_connect(m->ip, WS_PORT, 300);
    if (ch < 0) return -1;
    peer = chan_peer(ch);
    /* The channel authenticated a key; it must be the member's key. */
    if (!peer || !mem_eq_ct(peer, m->sign_pub, 32)) {
        chan_close(ch);
        return -1;
    }
    return ch;
}

/* ---- presence ---------------------------------------------------------- */

static int online_count(void) {
    int n = 0;
    for (int i = 0; i < MAXM; i++) n += st.m[i].used && (st.m[i].online || is_me(&st.m[i]));
    return n;
}

/* Any authenticated message from a member counts as a sign of life. */
static void seen(member_t *m) {
    m->last_seen = sh_ticks();
    if (!m->online) {
        m->online = 1;
        m->need_sync = 1; /* catch up on whatever changed while apart */
        event(m->name, " is online", 0);
    }
}

/* Ping every member with an open channel and start handshakes to the
 * rest; anyone silent for OFFLINE_TICKS is marked offline and their
 * channel dropped, so a restarted peer gets a fresh handshake. */
static void heartbeat(void) {
    uint8_t ping = M_PING;
    for (int i = 0; i < MAXM; i++) {
        member_t *m = &st.m[i];
        int ch;
        if (!m->used || is_me(m) || mem_eq_ct(m->ip, my_ip, 4)) continue;
        ch = chan_find(m->sign_pub);
        if (m->online && sh_ticks() - m->last_seen > OFFLINE_TICKS) {
            m->online = 0;
            if (ch >= 0) chan_close(ch);
            ch = -1;
            event(m->name, " went offline", 0);
        }
        if (ch >= 0) (void)chan_send(ch, &ping, 1);
        else (void)chan_open(m->ip, WS_PORT);
    }
}

/* ---- files ------------------------------------------------------------- *
 * Object layout:
 *   "SWF1" | file_id16 | owner_rank1 | signer_rank1 | size4
 *   | name: nonce24 ct64 tag16 | content: nonce24 ct(size) tag16
 *   | nbox1 | boxes: kind1 rank1 (SC_WRAP_BOX | 72 bytes) ...
 *   | sig64 (Ed25519 by the signer over SHA-256 of everything before)  */

static uint8_t obj[MAX_OBJECT];
static uint8_t obj2[MAX_OBJECT];
static uint8_t plain[MAX_CONTENT + 1];

#define OFF_ID     4
#define OFF_OWNER  20
#define OFF_SIGNER 21
#define OFF_SIZE   22
#define OFF_NAME   26
#define OFF_BODY   (OFF_NAME + 24 + 64 + 16)

static uint64_t box_len(uint8_t kind) {
    return kind == BOX_MEMBER ? SC_WRAP_BOX : 72;
}

static uint64_t boxes_off(const uint8_t *o) {
    return OFF_BODY + 24 + sc_load32_le(o + OFF_SIZE) + 16;
}

static uint64_t object_len(const uint8_t *o) {
    uint64_t p = boxes_off(o);
    int n = o[p++];
    for (int i = 0; i < n; i++) p += 2 + box_len(o[p]);
    return p + 64;
}

static void file_purpose(char *buf, const uint8_t id[16]) {
    static const char hx[] = "0123456789abcdef";
    str_copy(buf, "fk:", 48);
    for (int i = 0; i < 16; i++) {
        buf[3 + 2 * i] = hx[id[i] >> 4];
        buf[4 + 2 * i] = hx[id[i] & 15];
    }
    buf[35] = '\0';
}

static void file_path(char *buf, const uint8_t id[16]) {
    static const char hx[] = "0123456789abcdef";
    str_copy(buf, WS_DIR "/files/", 96);
    uint64_t n = str_len(buf);
    for (int i = 0; i < 16; i++) {
        buf[n + 2 * i] = hx[id[i] >> 4];
        buf[n + 2 * i + 1] = hx[id[i] & 15];
    }
    buf[n + 32] = '\0';
}

/* Verify an object's signature and authority.  Returns its length or -1. */
static long verify_object(const uint8_t *o, uint64_t avail) {
    uint8_t h[32];
    uint64_t len;
    member_t *owner, *signer;
    if (avail < OFF_BODY + 40 + 1 + 64 || sc_load32_le(o) != 0x31465753u) return -1;
    if (sc_load32_le(o + OFF_SIZE) > MAX_CONTENT) return -1;
    len = object_len(o);
    if (len > avail || len > MAX_OBJECT) return -1;
    owner = member_by_rank(o[OFF_OWNER]);
    signer = member_by_rank(o[OFF_SIGNER]);
    /* Signed by the owner, or by someone ranked above the owner. */
    if (!owner || !signer || signer->rank > owner->rank) return -1;
    sc_sha256(o, len - 64, h);
    if (sc_ed25519_verify(o + len - 64, h, 32, signer->sign_pub) != 0) return -1;
    return (long)len;
}

static long load_object(uint8_t *o, const uint8_t id[16]) {
    char path[96];
    long n;
    file_path(path, id);
    n = sh_read_at(path, 0, (char *)o, MAX_OBJECT);
    return n > 0 ? verify_object(o, (uint64_t)n) : -1;
}

/* Recover the file key, if this member is allowed to. */
static int file_key(uint8_t fk[32], const uint8_t *o) {
    uint64_t p = boxes_off(o);
    int n = o[p++];
    char purpose[48];
    file_purpose(purpose, o + OFF_ID);
    for (int i = 0; i < n; i++) {
        uint8_t kind = o[p], rank = o[p + 1];
        const uint8_t *b = o + p + 2;
        if (kind == BOX_MEMBER && rank == st.my_rank &&
            sc_unwrap(fk, b, &wrap_pub, &wrap_priv, (const uint8_t *)purpose, str_len(purpose)) == 0) {
            return 0;
        }
        if (kind == BOX_RANK && rank >= st.my_rank) {
            uint8_t rk[32];
            int ok = rank_key(rk, rank) == 0 && open_sym(fk, b, rk, purpose) == 0;
            mem_wipe(rk, 32);
            if (ok) return 0;
        }
        p += 2 + box_len(kind);
    }
    return -1;
}

static int decrypt_name(char name[65], const uint8_t *o, const uint8_t fk[32]) {
    if (sc_xaead_open((uint8_t *)name, o + OFF_NAME + 24, 64, o + OFF_NAME + 88, o + OFF_ID, 16,
                      o + OFF_NAME, fk) != 0) return -1;
    name[64] = '\0';
    return 0;
}

static void add_member_box(uint8_t *o, uint64_t *p, const member_t *m, const uint8_t fk[32], const char *purpose) {
    uint8_t rnd[SC_WRAP_RANDOM];
    rand_bytes(rnd, sizeof(rnd));
    o[(*p)++] = BOX_MEMBER;
    o[(*p)++] = m->rank;
    (void)sc_wrap(o + *p, &m->wrap, fk, (const uint8_t *)purpose, str_len(purpose), rnd);
    *p += SC_WRAP_BOX;
    mem_wipe(rnd, sizeof(rnd));
}

static void sign_object(uint8_t *o, uint64_t body_len) {
    uint8_t h[32];
    o[OFF_SIGNER] = (uint8_t)st.my_rank;
    sc_sha256(o, body_len, h);
    sc_ed25519_sign(o + body_len, h, 32, sign_seed, sign_pub);
}

static void store_object(const uint8_t *o, uint64_t len) {
    char path[96];
    file_path(path, o + OFF_ID);
    (void)sh_write_file(path, o, len);
}

/* ---- status file -------------------------------------------------------- */

static int nfiles, nlocked, files_dirty = 1;

static int hex_id(uint8_t id[16], const char *h) {
    for (int k = 0; k < 16; k++) {
        int v = 0;
        for (int j = 0; j < 2; j++) {
            char c = h[2 * k + j];
            int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
            if (d < 0) return -1;
            v = v * 16 + d;
        }
        id[k] = (uint8_t)v;
    }
    return 0;
}

/* Calls fn(id) for every stored object; stops early if fn returns 1. */
static void each_file(int (*fn)(const uint8_t id[16], void *arg), void *arg) {
    char names[2048];
    long n = sh_list_dir(WS_DIR "/files", names, sizeof(names));
    for (long i = 0; n > 0 && i < n;) {
        uint8_t id[16];
        long j = i;
        while (j < n && names[j] != '\n') j++;
        if (j - i == 32 && hex_id(id, names + i) == 0 && fn(id, arg)) return;
        i = j + 1;
    }
}

static int count_one(const uint8_t id[16], void *arg) {
    uint8_t fk[32];
    (void)arg;
    if (load_object(obj2, id) <= 0) return 0;
    nfiles++;
    if (file_key(fk, obj2) != 0) nlocked++;
    mem_wipe(fk, 32);
    return 0;
}

static void st_line(char *b, const char *k, const char *v) {
    str_cat(b, k, 512);
    str_cat(b, " ", 512);
    str_cat(b, v, 512);
    str_cat(b, "\n", 512);
}

/* One "key value" per line; the WM's top bar reads it.  Owned by the
 * workspace's user, mode 0600: other local users learn nothing. */
static void write_status(void) {
    static char last[512];
    char b[512], n[24], ip[16];
    b[0] = '\0';
    st_line(b, "active", st.active ? "1" : "0");
    st_line(b, "address", net_ip_text(ip, my_ip));
    if (st.active) {
        int nreq = st.nreq;
        if (files_dirty) {
            nfiles = nlocked = 0;
            each_file(count_one, 0);
            files_dirty = 0;
        }
        st_line(b, "name", st.ws_name);
        st_line(b, "rank", fmt_u64(n, (uint64_t)st.my_rank, 10));
        st_line(b, "members", fmt_u64(n, (uint64_t)member_count(), 10));
        st_line(b, "online", fmt_u64(n, (uint64_t)online_count(), 10));
        st_line(b, "requests", fmt_u64(n, (uint64_t)nreq, 10));
        st_line(b, "files", fmt_u64(n, (uint64_t)nfiles, 10));
        st_line(b, "locked", fmt_u64(n, (uint64_t)nlocked, 10));
        if (busy_text[0]) st_line(b, "busy", busy_text);
        if (nev) {
            str_cat(b, "event ", sizeof(b));
            str_cat(b, fmt_u64(n, evs[nev - 1].tick, 10), sizeof(b));
            st_line(b, "", evs[nev - 1].text);
        }
    }
    if (str_eq(b, last)) return;
    str_copy(last, b, sizeof(last));
    (void)sh_write_file(WS_DIR "/status", b, str_len(b));
    (void)sh_chown(WS_DIR "/status", st.active ? st.owner_uid : 0);
    (void)sh_chmod(WS_DIR "/status", st.active ? 0600 : 0644);
}

/* ---- peer message handling (never blocks) ------------------------------- */

static uint8_t rxbuf[MAX_OBJECT];
static uint8_t rx_id[16];
static uint32_t rx_total;

static void reply(int ch, const uint8_t *data, uint64_t len) {
    (void)chan_send(ch, data, len);
}

static void push_object(int ch, const uint8_t *o, uint64_t len) {
    uint8_t m[1 + 16 + 4 + 4 + CHUNK];
    for (uint64_t off = 0; off < len; off += CHUNK) {
        uint64_t n = len - off < CHUNK ? len - off : CHUNK;
        m[0] = M_FILE_CHUNK;
        mem_copy(m + 1, o + OFF_ID, 16);
        sc_store32_le(m + 17, (uint32_t)len);
        sc_store32_le(m + 21, (uint32_t)off);
        mem_copy(m + 25, o + off, n);
        (void)chan_send(ch, m, 25 + n);
        sh_sleep(1); /* pace bursts so the receiver's queue keeps up */
    }
}

static int sync_pending;
static uint8_t sync_from[4];

static void handle_peer(chan_msg_t *msg) {
    const uint8_t *d = msg->data;
    member_t *peer = member_by_pub(msg->peer_pub);
    uint8_t r[1400];
    int ch = msg->chan;

    if (d[0] == M_JOIN_REQ) {
        /* ws_id16 | proof32 | name24 | wrap_pub */
        uint8_t body[ENTRY_BODY], expect[32], rk[32], prk[32];
        char purpose[48];
        sc_wrap_pub_t w;
        member_t *prev;
        int inv = -1, rank;
        if (msg->len != 1 + 16 + 32 + 24 + sizeof(sc_wrap_pub_t) || !st.active || st.my_rank != 0 ||
            !mem_eq_ct(d + 1, st.ws_id, 8)) goto fail;
        for (int i = 0; i < MAXINV; i++) {
            /* The invite carries an 8-byte id prefix; the proof is over the
             * prefix zero-padded to 16 bytes, as the joiner computes it. */
            uint8_t in[16 + 32 + 4];
            if (st.inv[i].state != INV_OPEN) continue;
            mem_set(in, 0, 16);
            mem_copy(in, st.ws_id, 8);
            mem_copy(in + 16, msg->peer_pub, 32);
            mem_copy(in + 48, "join", 4);
            sc_hmac_sha256(st.inv[i].secret, 16, in, sizeof(in), expect);
            if (mem_eq_ct(expect, d + 17, 32)) {
                inv = i;
                break;
            }
        }
        if (inv < 0) {
            event("Refused a join with an invalid or used invite", 0, 0);
            goto fail;
        }
        if (member_by_pub(msg->peer_pub)) goto fail;
        rank = 0;
        for (int i = 0; i < MAXM; i++) {
            if (st.m[i].used && st.m[i].rank >= rank) rank = st.m[i].rank + 1;
        }
        if (rank >= MAXM) goto fail;
        mem_copy(&w, d + 73, sizeof(w));
        /* New rank key, chained under the rank above. */
        rand_bytes(rk, 32);
        prev = member_by_rank(rank - 1);
        if (!prev || rank_key(prk, rank - 1) != 0) goto fail;
        mem_set(body, 0, sizeof(body));
        body[0] = (uint8_t)rank;
        mem_copy(body + 1, d + 49, 24);
        body[24] = '\0';
        mem_copy(body + 25, msg->peer_pub, 32);
        wrap_hash_of(body + 57, &w);
        rk_purpose(purpose, rank);
        seal_sym(body + 89, prk, rk, purpose);
        mem_copy(body + 161, msg->from.ip, 4);
        st.inv[inv].state = INV_USED;
        if (append_entry(E_JOIN, body) != 0) goto fail;
        {
            member_t *m = member_by_rank(rank);
            m->have_wrap = 1;
            m->wrap = w;
        }
        save_state();
        {
            char rn[8];
            event((const char *)body + 1, " joined as rank ", fmt_u64(rn, (uint64_t)rank, 10));
        }
        r[0] = M_JOIN_OK;
        r[1] = (uint8_t)rank;
        {
            uint8_t rnd[SC_WRAP_RANDOM];
            rand_bytes(rnd, sizeof(rnd));
            (void)sc_wrap(r + 2, &w, rk, (const uint8_t *)purpose, str_len(purpose), rnd);
        }
        reply(ch, r, 2 + SC_WRAP_BOX);
        mem_wipe(rk, 32);
        mem_wipe(prk, 32);
        log_line("member joined");
        /* Tell everyone else to pull the new log entry. */
        sync_pending = 2;
        return;
    fail:
        r[0] = M_JOIN_FAIL;
        reply(ch, r, 1);
        return;
    }

    /* Everything else is for members only. */
    if (!peer) return;
    if (!mem_eq_ct(peer->ip, msg->from.ip, 4)) mem_copy(peer->ip, msg->from.ip, 4);
    seen(peer);

    switch (d[0]) {
    case M_PING:
        r[0] = M_PONG;
        reply(ch, r, 1);
        break;
    case M_PONG:
        break;
    case M_LOG_GET: {
        uint32_t seq = sc_load32_le(d + 1);
        if (seq < st.log_len) {
            r[0] = M_LOG_ENTRY;
            mem_copy(r + 1, st.log[seq], ENTRY_SIZE);
            reply(ch, r, 1 + ENTRY_SIZE);
        } else {
            r[0] = M_LOG_END;
            sc_store32_le(r + 1, st.log_len);
            reply(ch, r, 5);
        }
        break;
    }
    case M_WRAP_GET: {
        member_t *m = member_by_rank(d[1]);
        if (!m || !m->have_wrap) {
            r[0] = M_JOIN_FAIL;
            reply(ch, r, 1);
            break;
        }
        r[0] = M_WRAP;
        r[1] = m->rank;
        mem_copy(r + 2, &m->wrap, sizeof(m->wrap));
        reply(ch, r, 2 + sizeof(m->wrap));
        break;
    }
    case M_NOTIFY:
        sync_pending = 1;
        mem_copy(sync_from, msg->from.ip, 4);
        break;
    case M_FILE_CHUNK: {
        uint32_t total = sc_load32_le(d + 17), off = sc_load32_le(d + 21), n = msg->len - 25u;
        if (total > MAX_OBJECT || off + n > total) break;
        if (off == 0 || !mem_eq_ct(rx_id, d + 1, 16)) {
            mem_copy(rx_id, d + 1, 16);
            rx_total = total;
        }
        mem_copy(rxbuf + off, d + 25, n);
        if (off + n == rx_total) {
            long len = verify_object(rxbuf, rx_total);
            if (len > 0 && mem_eq_ct(rxbuf + OFF_ID, rx_id, 16)) {
                uint8_t fk[32];
                char name[65];
                int had = load_object(obj2, rx_id) > 0, had_key = had && file_key(fk, obj2) == 0;
                int now_key = file_key(fk, rxbuf) == 0 && decrypt_name(name, rxbuf, fk) == 0;
                member_t *owner = member_by_rank(rxbuf[OFF_OWNER]);
                mem_wipe(fk, 32);
                store_object(rxbuf, (uint64_t)len);
                files_dirty = 1;
                r[0] = M_ACK;
                reply(ch, r, 1);
                if (!had) event(owner ? owner->name : "someone", now_key ? " shared " : " shared a locked file",
                                now_key ? name : 0);
                else if (!had_key && now_key) event("You can now open ", name, 0);
                else write_status();
            } else {
                log_line("dropped a file object with a bad signature");
            }
        }
        break;
    }
    case M_FILE_LIST_GET: {
        char names[2048];
        long n = sh_list_dir(WS_DIR "/files", names, sizeof(names));
        uint64_t p = 2;
        r[0] = M_FILE_LIST;
        r[1] = 0;
        for (long i = 0; n > 0 && i < n && p + 16 <= CHAN_MAX_MSG && r[1] < 64; ) {
            /* each line: 32 hex chars */
            if (n - i >= 32) {
                for (int k = 0; k < 16; k++) {
                    uint8_t hi = (uint8_t)names[i + 2 * k], lo = (uint8_t)names[i + 2 * k + 1];
                    hi = (uint8_t)(hi <= '9' ? hi - '0' : hi - 'a' + 10);
                    lo = (uint8_t)(lo <= '9' ? lo - '0' : lo - 'a' + 10);
                    r[p + k] = (uint8_t)(hi << 4 | lo);
                }
                p += 16;
                r[1]++;
            }
            while (i < n && names[i] != '\n') i++;
            i++;
        }
        reply(ch, r, p);
        break;
    }
    case M_FILE_GET: {
        long len = load_object(obj2, d + 1);
        if (len > 0) push_object(ch, obj2, (uint64_t)len);
        break;
    }
    case M_ACCESS_REQ: {
        int dup = 0;
        for (int i = 0; i < st.nreq; i++) {
            dup |= st.req[i].rank == peer->rank && mem_eq_ct(st.req[i].file_id, d + 1, 16);
        }
        if (!dup && st.nreq < MAXREQ) {
            uint8_t fk[32];
            char name[65];
            mem_copy(st.req[st.nreq].file_id, d + 1, 16);
            st.req[st.nreq].rank = peer->rank;
            st.nreq++;
            save_state();
            if (load_object(obj2, d + 1) > 0 && file_key(fk, obj2) == 0 && decrypt_name(name, obj2, fk) == 0) {
                event(peer->name, " asks for ", name);
            } else {
                event(peer->name, " asks for access to a file", 0);
            }
            mem_wipe(fk, 32);
        }
        r[0] = M_ACK;
        reply(ch, r, 1);
        break;
    }
    }
}

/* ---- sync -------------------------------------------------------------- */

static int fetch_wrap(int ch, member_t *m) {
    uint8_t req[2] = { M_WRAP_GET, m->rank }, h[32];
    if (rpc(ch, req, 2, M_WRAP, &rx, 150) != 0 || rx.len != 2 + sizeof(sc_wrap_pub_t)) return -1;
    wrap_hash_of(h, (const sc_wrap_pub_t *)(rx.data + 2));
    if (!mem_eq_ct(h, m->wrap_hash, 32)) return -1; /* must match the signed log */
    mem_copy(&m->wrap, rx.data + 2, sizeof(m->wrap));
    m->have_wrap = 1;
    return 0;
}

/* Pull new log entries, missing wrap keys and missing files from a peer. */
static int sync_with(int ch) {
    uint8_t req[5];
    int added = 0;
    uint32_t first_new = st.log_len;
    for (;;) {
        req[0] = M_LOG_GET;
        sc_store32_le(req + 1, st.log_len);
        if (rpc(ch, req, 5, M_LOG_ENTRY, &rx, 150) != 0) break;
        if (st.log_len >= MAXLOG || rx.len != 1 + ENTRY_SIZE) return -1;
        mem_copy(st.log[st.log_len], rx.data + 1, ENTRY_SIZE);
        st.log_len++;
        if (replay_log() != 0) {
            st.log_len--;
            log_line("rejected a log entry that does not verify");
            return -1;
        }
        added++;
    }
    if (st.active) {
        for (uint32_t e = first_new; e < st.log_len; e++) {
            const uint8_t *body = st.log[e] + 5;
            char rn[8];
            if (st.log[e][4] == E_JOIN && !mem_eq_ct(body + 25, sign_pub, 32)) {
                event((const char *)body + 1, " joined as rank ", fmt_u64(rn, body[0], 10));
            }
        }
    }
    for (int i = 0; i < MAXM; i++) {
        if (st.m[i].used && !st.m[i].have_wrap) (void)fetch_wrap(ch, &st.m[i]);
    }
    /* Files we do not have yet. */
    req[0] = M_FILE_LIST_GET;
    if (rpc(ch, req, 1, M_FILE_LIST, &rx, 150) == 0) {
        uint8_t ids[64][16];
        int n = rx.data[1];
        if (n > 64) n = 64;
        mem_copy(ids, rx.data + 2, (uint64_t)n * 16);
        for (int i = 0; i < n; i++) {
            uint8_t get[17];
            uint64_t end;
            if (load_object(obj2, ids[i]) > 0) continue;
            get[0] = M_FILE_GET;
            mem_copy(get + 1, ids[i], 16);
            (void)chan_send(ch, get, 17);
            end = sh_ticks() + 300;
            while (sh_ticks() < end && load_object(obj2, ids[i]) <= 0) {
                if (chan_poll(&rx)) handle_peer(&rx);
                else sh_sleep(1);
            }
        }
    }
    save_state();
    return added;
}

static void notify_all(void) {
    uint8_t n = M_NOTIFY;
    for (int i = 0; i < MAXM; i++) {
        int ch;
        if (!st.m[i].used || mem_eq_ct(st.m[i].sign_pub, sign_pub, 32)) continue;
        ch = connect_member(&st.m[i]);
        if (ch >= 0) (void)chan_send(ch, &n, 1);
    }
}

/* ---- invite codes ------------------------------------------------------ */

static const char B32[] = "abcdefghijklmnopqrstuvwxyz234567";

static void b32_encode(char *outp, const uint8_t *in, int n) {
    uint32_t acc = 0;
    int bits = 0, o = 0;
    for (int i = 0; i < n; i++) {
        acc = (acc << 8) | in[i];
        bits += 8;
        while (bits >= 5) {
            outp[o++] = B32[(acc >> (bits - 5)) & 31];
            bits -= 5;
        }
    }
    if (bits) outp[o++] = B32[(acc << (5 - bits)) & 31];
    outp[o] = '\0';
}

static int b32_decode(uint8_t *outp, int n, const char *in) {
    uint32_t acc = 0;
    int bits = 0, o = 0;
    for (; *in && o < n; in++) {
        int v = -1;
        for (int k = 0; k < 32; k++) {
            if (B32[k] == *in) v = k;
        }
        if (v < 0) return -1;
        acc = (acc << 5) | (uint32_t)v;
        bits += 5;
        if (bits >= 8) {
            outp[o++] = (uint8_t)(acc >> (bits - 8));
            bits -= 8;
        }
    }
    return o == n ? 0 : -1;
}

/* ---- commands ---------------------------------------------------------- */

/* The local user making the current request (kernel-stamped). */
static uint32_t req_uid;
static int raw_ok; /* `raw` succeeded: the output is file content */
static const uint8_t *req_payload;
static uint64_t req_payload_len;

static void cmd_status(void) {
    char ip[16];
    say("device   ");
    say_hex(sign_pub, 6);
    say("\naddress  ");
    say(net_ip_text(ip, my_ip));
    say("\n");
    if (!st.active) {
        say("workspace: none (ws create <name> | ws join <code>)\n");
        return;
    }
    say("workspace ");
    say(st.ws_name);
    say("  (");
    say_u((uint64_t)member_count());
    say(" members)\nyour rank ");
    say_u((uint64_t)st.my_rank);
    say(st.my_rank == 0 ? " (admin)\n" : "\n");
}

static void cmd_create(const char *name) {
    uint8_t body[ENTRY_BODY];
    if (st.active) {
        say("already in a workspace\n");
        return;
    }
    mem_set(body, 0, sizeof(body));
    rand_bytes(body, 16);
    str_copy((char *)body + 16, name, 32);
    mem_copy(body + 48, sign_pub, 32);
    wrap_hash_of(body + 80, &wrap_pub);
    str_copy((char *)body + 112, "admin", 24);
    mem_copy(body + 136, my_ip, 4);
    st.log_len = 0;
    if (append_entry(E_GENESIS, body) != 0) {
        say("could not create the workspace\n");
        return;
    }
    st.active = 1;
    st.owner_uid = req_uid;
    st.my_rank = 0;
    rand_bytes(st.my_rk, 32);
    files_dirty = 1;
    save_state();
    event("Created workspace ", st.ws_name, 0);
    say("created workspace ");
    say(st.ws_name);
    say(" - you are rank 0 (admin)\n");
}

static void cmd_invite(void) {
    uint8_t raw[30];
    char code[64];
    int i;
    if (!st.active || st.my_rank != 0) {
        say("only the admin can invite\n");
        return;
    }
    /* A free slot, else a spent one; with all slots open the oldest
     * outstanding invite (slot 0) is revoked. */
    for (i = 0; i < MAXINV && st.inv[i].state != INV_FREE; i++) {}
    if (i == MAXINV) {
        for (i = 0; i < MAXINV && st.inv[i].state != INV_USED; i++) {}
    }
    if (i == MAXINV) i = 0;
    rand_bytes(st.inv[i].secret, 16);
    st.inv[i].state = INV_OPEN;
    save_state();
    mem_copy(raw, my_ip, 4);
    raw[4] = (uint8_t)WS_PORT;
    raw[5] = (uint8_t)(WS_PORT >> 8);
    mem_copy(raw + 6, st.ws_id, 8);
    mem_copy(raw + 14, st.inv[i].secret, 16);
    str_copy(code, "shu", sizeof(code));
    b32_encode(code + 3, raw, 30);
    say("invite code (single use):\n");
    say(code);
    say("\n");
}

static void cmd_join(const char *code, const char *name) {
    uint8_t raw[30], req[1 + 16 + 32 + 24 + sizeof(sc_wrap_pub_t)], in[52], ip[4];
    char purpose[48];
    int ch;
    if (st.active) {
        say("already in a workspace\n");
        return;
    }
    if (!str_starts(code, "shu") || b32_decode(raw, 30, code + 3) != 0) {
        say("that is not a valid invite code\n");
        return;
    }
    mem_copy(ip, raw, 4);
    set_busy("Joining a workspace", 0);
    ch = chan_connect(ip, (uint16_t)(raw[4] | raw[5] << 8), 400);
    set_busy(0, 0);
    if (ch < 0) {
        say("cannot reach the workspace admin\n");
        return;
    }
    /* Proof of the invite secret, bound to our authenticated identity.  We
     * only know the 8-byte id prefix from the invite (the full id comes
     * with the log), so the proof covers the prefix zero-padded. */
    req[0] = M_JOIN_REQ;
    mem_set(req + 1, 0, 16);
    mem_copy(req + 1, raw + 6, 8);
    mem_copy(in, req + 1, 16);
    mem_copy(in + 16, sign_pub, 32);
    mem_copy(in + 48, "join", 4);
    sc_hmac_sha256(raw + 14, 16, in, sizeof(in), req + 17);
    mem_set(req + 49, 0, 24);
    str_copy((char *)req + 49, name, 24);
    mem_copy(req + 73, &wrap_pub, sizeof(wrap_pub));
    if (rpc(ch, req, sizeof(req), M_JOIN_OK, &rx, 300) != 0 || rx.len != 2 + SC_WRAP_BOX) {
        say("the admin refused the join (invite invalid or already used)\n");
        return;
    }
    st.my_rank = rx.data[1];
    rk_purpose(purpose, st.my_rank);
    if (sc_unwrap(st.my_rk, rx.data + 2, &wrap_pub, &wrap_priv, (const uint8_t *)purpose, str_len(purpose)) != 0) {
        say("could not open the rank key\n");
        return;
    }
    st.log_len = 0;
    if (sync_with(ch) <= 0 || !member_by_pub(sign_pub)) {
        say("could not verify the workspace log\n");
        return;
    }
    st.active = 1;
    st.owner_uid = req_uid;
    files_dirty = 1;
    {
        member_t *admin = member_by_rank(0);
        if (admin) seen(admin); /* we just talked to it */
    }
    save_state();
    event("Joined workspace ", st.ws_name, 0);
    say("joined workspace ");
    say(st.ws_name);
    say(" as rank ");
    say_u((uint64_t)st.my_rank);
    say("\n");
}

static void cmd_members(void) {
    char ip[16];
    if (!st.active) {
        say("no workspace\n");
        return;
    }
    for (int r = 0; r < MAXM; r++) {
        member_t *m = member_by_rank(r);
        if (!m) continue;
        say("  rank ");
        say_u((uint64_t)r);
        say("  ");
        say(m->name);
        say("  ");
        say(net_ip_text(ip, m->ip));
        say("  key ");
        say_hex(m->sign_pub, 6);
        if (mem_eq_ct(m->sign_pub, sign_pub, 32)) say("  (you)");
        say("\n");
    }
}

/* Push an object and wait for the receiver to confirm it verified and
 * stored it; a cached channel to a machine that has gone away times out
 * here instead of counting as delivered. */
static int push_acked(int ch, const uint8_t *o, uint64_t len) {
    uint64_t end;
    push_object(ch, o, len);
    end = sh_ticks() + 200;
    while (sh_ticks() < end) {
        if (!chan_poll(&rx)) {
            sh_sleep(1);
            continue;
        }
        if (rx.chan == ch && rx.data[0] == M_ACK) return 0;
        handle_peer(&rx);
    }
    return -1;
}

static void replicate(const uint8_t *o, uint64_t len) {
    int sent = 0, others = 0;
    for (int i = 0; i < MAXM; i++) {
        int ch;
        if (!st.m[i].used || mem_eq_ct(st.m[i].sign_pub, sign_pub, 32)) continue;
        others++;
        ch = connect_member(&st.m[i]);
        if (ch < 0) continue;
        set_busy("Sending to ", st.m[i].name);
        if (push_acked(ch, o, len) == 0) sent++;
    }
    set_busy(0, 0);
    say("replicated to ");
    say_u((uint64_t)sent);
    say(" of ");
    say_u((uint64_t)others);
    say(" member(s)");
    say(sent < others ? " - the rest pick it up on their next sync\n" : "\n");
}

/* Share a copy of a file the user handed over.  The client read it with
 * the user's own permissions; the daemon never opens personal paths. */
static void cmd_put(const char *base) {
    uint8_t fk[32];
    char name[65], purpose[48];
    long size = (long)req_payload_len;
    uint64_t p;
    member_t *me = member_by_pub(sign_pub);
    if (!st.active || !me) {
        say("no workspace\n");
        return;
    }
    if (!req_payload || size > MAX_CONTENT) {
        say(req_payload ? "file too large (8 KB limit for now)\n" : "nothing to share\n");
        return;
    }
    for (const char *c = base; *c; c++) {
        if (*c == '/' || *c < 32) {
            say("bad file name\n");
            return;
        }
    }
    mem_copy(plain, req_payload, (uint64_t)size);
    mem_set(name, 0, sizeof(name));
    str_copy(name, base, sizeof(name));
    rand_bytes(fk, 32);
    sc_store32_le(obj, 0x31465753u);
    rand_bytes(obj + OFF_ID, 16);
    obj[OFF_OWNER] = (uint8_t)st.my_rank;
    sc_store32_le(obj + OFF_SIZE, (uint32_t)size);
    rand_bytes(obj + OFF_NAME, 24);
    sc_xaead_seal(obj + OFF_NAME + 24, obj + OFF_NAME + 88, (uint8_t *)name, 64, obj + OFF_ID, 16, obj + OFF_NAME, fk);
    rand_bytes(obj + OFF_BODY, 24);
    sc_xaead_seal(obj + OFF_BODY + 24, obj + OFF_BODY + 24 + size, plain, (uint64_t)size, obj + OFF_ID, 16,
                  obj + OFF_BODY, fk);
    p = boxes_off(obj);
    file_purpose(purpose, obj + OFF_ID);
    obj[p++] = 0;
    add_member_box(obj, &p, me, fk, purpose);
    obj[boxes_off(obj)]++;
    if (st.my_rank > 0) {
        /* Seal FK under our own rank key: every higher rank derives RK_r by
         * walking the chain down, nobody below us holds it. */
        obj[p++] = BOX_RANK;
        obj[p++] = (uint8_t)st.my_rank;
        seal_sym(obj + p, st.my_rk, fk, purpose);
        p += 72;
        obj[boxes_off(obj)]++;
    }
    mem_wipe(fk, 32);
    sign_object(obj, p);
    store_object(obj, p + 64);
    files_dirty = 1;
    say("shared ");
    say(name);
    say(" (");
    say_u((uint64_t)size);
    say(" bytes, encrypted)\n");
    event("You shared ", name, 0);
    replicate(obj, p + 64);
}

static void cmd_files(void) {
    char names[2048];
    long n = sh_list_dir(WS_DIR "/files", names, sizeof(names));
    int count = 0;
    if (!st.active) {
        say("no workspace\n");
        return;
    }
    for (long i = 0; n > 0 && i < n;) {
        uint8_t id[16], fk[32];
        char name[65];
        long j = i;
        while (j < n && names[j] != '\n') j++;
        if (j - i == 32) {
            for (int k = 0; k < 16; k++) {
                uint8_t hi = (uint8_t)names[i + 2 * k], lo = (uint8_t)names[i + 2 * k + 1];
                id[k] = (uint8_t)(((hi <= '9' ? hi - '0' : hi - 'a' + 10) << 4) | (lo <= '9' ? lo - '0' : lo - 'a' + 10));
            }
            if (load_object(obj, id) > 0) {
                count++;
                say("  ");
                if (file_key(fk, obj) == 0 && decrypt_name(name, obj, fk) == 0) {
                    say(name);
                    say("  owner rank ");
                    say_u(obj[OFF_OWNER]);
                    say("  [open]\n");
                } else {
                    say("(locked file)  owner rank ");
                    say_u(obj[OFF_OWNER]);
                    say("  [no access - ws request]\n");
                }
                mem_wipe(fk, 32);
            }
        }
        i = j + 1;
    }
    if (!count) say("  no shared files\n");
}

/* Find a file object by its (decrypted) name, or for locked files by the
 * owner rank given as "rank<N>".  Fills obj; returns 0, or -1 if nothing
 * matched (*have_key then says whether any locked file was seen). */
static int find_file(const char *want, uint8_t fk[32], int *have_key) {
    char names[2048];
    long n = sh_list_dir(WS_DIR "/files", names, sizeof(names));
    int locked = 0;
    for (long i = 0; n > 0 && i < n;) {
        uint8_t id[16];
        char name[65];
        long j = i;
        while (j < n && names[j] != '\n') j++;
        if (j - i == 32) {
            for (int k = 0; k < 16; k++) {
                uint8_t hi = (uint8_t)names[i + 2 * k], lo = (uint8_t)names[i + 2 * k + 1];
                id[k] = (uint8_t)(((hi <= '9' ? hi - '0' : hi - 'a' + 10) << 4) | (lo <= '9' ? lo - '0' : lo - 'a' + 10));
            }
            if (load_object(obj, id) > 0) {
                uint8_t want_id[16];
                *have_key = file_key(fk, obj) == 0;
                if (want[0] == '#' && hex_id(want_id, want + 1) == 0 && mem_eq_ct(want_id, id, 16)) return 0;
                if (*have_key && decrypt_name(name, obj, fk) == 0 && str_eq(name, want)) return 0;
                if (!*have_key && str_starts(want, "rank") && obj[OFF_OWNER] == (uint8_t)(want[4] - '0')) return 0;
                locked |= !*have_key;
            }
        }
        i = j + 1;
    }
    *have_key = locked;
    return -1;
}

/* raw: exact bytes, for `ws get` and the Workspace app. */
static void cmd_cat(const char *name, int raw) {
    uint8_t fk[32];
    int have;
    uint32_t size;
    if (find_file(name, fk, &have) != 0) {
        /* Names are encrypted too, so a locked file cannot be matched. */
        say(have ? "no readable file by that name - locked files (names hidden) are listed by ws files\n"
                 : "no such shared file\n");
        return;
    }
    if (!have) {
        say("no access: this file is locked for your rank (ws request ");
        say(name);
        say(")\n");
        return;
    }
    size = sc_load32_le(obj + OFF_SIZE);
    if (sc_xaead_open(plain, obj + OFF_BODY + 24, size, obj + OFF_BODY + 24 + size, obj + OFF_ID, 16,
                      obj + OFF_BODY, fk) != 0) {
        say("file failed authentication\n");
    } else if (raw) {
        say_bytes(plain, size);
        raw_ok = 1;
    } else {
        plain[size] = '\0';
        say((char *)plain);
        if (size && plain[size - 1] != '\n') say("\n");
    }
    mem_wipe(fk, 32);
    mem_wipe(plain, size);
}

static void cmd_request(const char *name) {
    uint8_t fk[32], req[18];
    int have, ch;
    member_t *owner;
    if (find_file(name, fk, &have) != 0) {
        say("no such shared file (locked files are named rank<N>, see ws files)\n");
        return;
    }
    if (have) {
        say("you can already open it\n");
        return;
    }
    owner = member_by_rank(obj[OFF_OWNER]);
    ch = owner ? connect_member(owner) : -1;
    if (ch < 0) {
        say("the owner is not reachable\n");
        return;
    }
    req[0] = M_ACCESS_REQ;
    mem_copy(req + 1, obj + OFF_ID, 16);
    if (rpc(ch, req, 17, M_ACK, &rx, 150) != 0) {
        say("request not delivered\n");
        return;
    }
    say("access requested from rank ");
    say_u(obj[OFF_OWNER]);
    say("\n");
    event("Asked ", owner->name, " for access");
}

static void cmd_requests(void) {
    if (!st.nreq) {
        say("no access requests\n");
        return;
    }
    for (int i = 0; i < st.nreq; i++) {
        uint8_t fk[32];
        char name[65];
        say("  rank ");
        say_u(st.req[i].rank);
        say(" asks for ");
        if (load_object(obj, st.req[i].file_id) > 0 && file_key(fk, obj) == 0 && decrypt_name(name, obj, fk) == 0) {
            say(name);
        } else {
            say("(unknown file)");
        }
        say("   -> ws grant <file> ");
        say_u(st.req[i].rank);
        say("\n");
    }
}

static void cmd_grant(const char *name, int rank) {
    uint8_t fk[32];
    char purpose[48], fname[65];
    int have;
    uint64_t p, body_len;
    member_t *m = member_by_rank(rank);
    if (find_file(name, fk, &have) != 0 || !have) {
        say("you cannot open that file, so you cannot grant it\n");
        return;
    }
    /* Only the owner or a higher rank may grant. */
    if (st.my_rank > obj[OFF_OWNER]) {
        say("only the owner or a higher rank can grant access\n");
        return;
    }
    if (!m || !m->have_wrap) {
        say("unknown member (try ws sync)\n");
        return;
    }
    if (decrypt_name(fname, obj, fk) != 0) str_copy(fname, name, sizeof(fname));
    body_len = object_len(obj) - 64;
    p = body_len;
    file_purpose(purpose, obj + OFF_ID);
    add_member_box(obj, &p, m, fk, purpose);
    obj[boxes_off(obj)]++;
    sign_object(obj, p);
    store_object(obj, p + 64);
    for (int i = 0; i < st.nreq; i++) {
        if (st.req[i].rank == rank && mem_eq_ct(st.req[i].file_id, obj + OFF_ID, 16)) {
            st.req[i] = st.req[--st.nreq];
            i--;
        }
    }
    save_state();
    mem_wipe(fk, 32);
    say("granted ");
    say(fname);
    say(" to ");
    say(m->name);
    say(" (rank ");
    say_u((uint64_t)rank);
    say(")\n");
    files_dirty = 1;
    event("You granted ", m->name, " access");
    replicate(obj, p + 64);
}

static void cmd_sync(void) {
    int done = 0;
    if (!st.active) {
        say("no workspace\n");
        return;
    }
    for (int i = 0; i < MAXM; i++) {
        int ch;
        if (!st.m[i].used || mem_eq_ct(st.m[i].sign_pub, sign_pub, 32)) continue;
        ch = connect_member(&st.m[i]);
        if (ch < 0) continue;
        set_busy("Syncing with ", st.m[i].name);
        if (sync_with(ch) >= 0) done++;
    }
    set_busy(0, 0);
    files_dirty = 1;
    write_status();
    say("synced with ");
    say_u((uint64_t)done);
    say(" member(s)\n");
}

static void cmd_deny(const char *name, int rank) {
    uint8_t fk[32];
    int have, n = st.nreq;
    if (find_file(name, fk, &have) != 0) {
        say("no such shared file\n");
        return;
    }
    mem_wipe(fk, 32);
    for (int i = 0; i < st.nreq; i++) {
        if (st.req[i].rank == rank && mem_eq_ct(st.req[i].file_id, obj + OFF_ID, 16)) {
            st.req[i] = st.req[--st.nreq];
            i--;
        }
    }
    save_state();
    write_status();
    say(n != st.nreq ? "request dismissed\n" : "no such request\n");
}

static void q_tab(const char *s) {
    say(s);
    say("\t");
}

static void q_status(void) {
    char ip[16];
    say("active\t");
    say(st.active ? "1\n" : "0\n");
    say("address\t");
    say(net_ip_text(ip, my_ip));
    say("\ndevice\t");
    say_hex(sign_pub, 6);
    say("\n");
    if (!st.active) return;
    say("name\t");
    say(st.ws_name);
    say("\nrank\t");
    say_u((uint64_t)st.my_rank);
    say("\nmembers\t");
    say_u((uint64_t)member_count());
    say("\nonline\t");
    say_u((uint64_t)online_count());
    say("\nrequests\t");
    say_u((uint64_t)st.nreq);
    say("\n");
}

static void q_members(void) {
    char ip[16];
    for (int r = 0; r < MAXM; r++) {
        member_t *m = member_by_rank(r);
        if (!m) continue;
        say_u((uint64_t)r);
        say("\t");
        q_tab(m->name);
        q_tab(net_ip_text(ip, m->ip));
        q_tab(is_me(m) ? "you" : m->online ? "online" : "offline");
        say_hex(m->sign_pub, 6);
        say("\n");
    }
}

/* id, name (empty if locked), owner rank, owner name, open?, size */
static int q_file_one(const uint8_t id[16], void *arg) {
    uint8_t fk[32];
    char name[65];
    member_t *owner;
    int open;
    (void)arg;
    if (load_object(obj, id) <= 0) return 0;
    open = file_key(fk, obj) == 0 && decrypt_name(name, obj, fk) == 0;
    mem_wipe(fk, 32);
    owner = member_by_rank(obj[OFF_OWNER]);
    say_hex(id, 16);
    say("\t");
    q_tab(open ? name : "");
    say_u(obj[OFF_OWNER]);
    say("\t");
    q_tab(owner ? owner->name : "?");
    q_tab(open ? "open" : "locked");
    say_u(sc_load32_le(obj + OFF_SIZE));
    say("\n");
    return 0;
}

static void q_requests(void) {
    for (int i = 0; i < st.nreq; i++) {
        uint8_t fk[32];
        char name[65];
        member_t *m = member_by_rank(st.req[i].rank);
        say_u(st.req[i].rank);
        say("\t");
        q_tab(m ? m->name : "?");
        say_hex(st.req[i].file_id, 16);
        say("\t");
        if (load_object(obj, st.req[i].file_id) > 0 && file_key(fk, obj) == 0 && decrypt_name(name, obj, fk) == 0) {
            say(name);
        }
        mem_wipe(fk, 32);
        say("\n");
    }
}

/* Newest first: seconds ago, text. */
static void q_events(void) {
    for (int i = nev - 1; i >= 0; i--) {
        say_u((sh_ticks() - evs[i].tick) / 100);
        say("\t");
        say(evs[i].text);
        say("\n");
    }
}

static void run_command(char *line) {
    char *argv[8];
    int argc = split_args(line, argv, 8);
    out_len = 0;
    out[0] = '\0';
    /* One workspace per machine, owned by the user who made or joined it.
     * Anyone else is told nothing beyond that. */
    if (st.active && req_uid != st.owner_uid) {
        if (argc > 1 && str_eq(argv[0], "q") && str_eq(argv[1], "status")) say("active\t0\nforeign\t1\n");
        else say("This machine's workspace belongs to another user.\n");
        return;
    }
    if (argc > 1 && str_eq(argv[0], "q")) {
        if (str_eq(argv[1], "status")) q_status();
        else if (str_eq(argv[1], "members")) q_members();
        else if (str_eq(argv[1], "files")) each_file(q_file_one, 0);
        else if (str_eq(argv[1], "requests")) q_requests();
        else if (str_eq(argv[1], "events")) q_events();
        return;
    }
    if (!argc || str_eq(argv[0], "status")) cmd_status();
    else if (str_eq(argv[0], "create") && argc > 1) cmd_create(argv[1]);
    else if (str_eq(argv[0], "invite")) cmd_invite();
    else if (str_eq(argv[0], "join") && argc > 1) cmd_join(argv[1], argc > 2 ? argv[2] : "member");
    else if (str_eq(argv[0], "members")) cmd_members();
    else if (str_eq(argv[0], "put") && argc > 1) cmd_put(argv[1]);
    else if (str_eq(argv[0], "files")) cmd_files();
    else if (str_eq(argv[0], "cat") && argc > 1) cmd_cat(argv[1], 0);
    else if (str_eq(argv[0], "raw") && argc > 1) cmd_cat(argv[1], 1);
    else if (str_eq(argv[0], "deny") && argc > 2) cmd_deny(argv[1], (int)(argv[2][0] - '0'));
    else if (str_eq(argv[0], "request") && argc > 1) cmd_request(argv[1]);
    else if (str_eq(argv[0], "requests")) cmd_requests();
    else if (str_eq(argv[0], "grant") && argc > 2) cmd_grant(argv[1], (int)(argv[2][0] - '0'));
    else if (str_eq(argv[0], "sync")) cmd_sync();
    else {
        say("usage: ws status | create <name> | invite | join <code> [name] | members\n"
            "          put <file> | get <file> <dest> | files | cat <file> | request <file>\n"
            "          requests | grant <file> <rank> | deny <file> <rank> | sync\n");
    }
}

/* ---- main -------------------------------------------------------------- */

int main(void) {
    uint8_t mask[4];
    uint64_t ctl;

    (void)sh_mkdir("/var");
    (void)sh_mkdir(WS_DIR);
    (void)sh_mkdir(WS_IO_DIR);
    (void)sh_mkdir(WS_DIR "/files");
    /* /var/ws/io is the drop box for `ws` requests from any user; the
     * identity, state and replicas stay root-only. */
    (void)sh_chmod("/var", 0755); /* user-created nodes default to private */
    (void)sh_chmod(WS_DIR, 0711);
    (void)sh_chmod(WS_IO_DIR, 0777);
    (void)sh_chmod(WS_DIR "/files", 0700);

    for (int tries = 0; net_local(my_ip, mask) != 0; tries++) {
        if (tries > 50) {
            log_line("no network; workspace disabled");
            return 1;
        }
        sh_sleep(10);
    }
    load_or_create_identity();
    load_state();
    if (st.active && replay_log() != 0) {
        log_line("stored log failed verification; leaving workspace");
        mem_set(&st, 0, sizeof(st));
        st.magic = STATE_MAGIC;
    }
    if (chan_init(sign_seed, sign_pub, WS_PORT) != 0) {
        log_line("cannot bind the workspace port");
        return 1;
    }
    ctl = sh_msg_open(WS_CTL_QUEUE);
    if (!ctl || sh_msg_owner(ctl) != (long)sh_pid()) {
        log_line("control queue is owned by another process");
        return 1;
    }
    log_line("ready");
    for (int i = 0; i < MAXM; i++) {
        st.m[i].online = 0;
        st.m[i].last_seen = 0;
        st.m[i].need_sync = 0;
    }
    write_status();

    for (;;) {
        static uint8_t inbuf[256 + MAX_CONTENT + 1];
        static uint64_t last_beat;
        ws_ctl_t m;
        long cred;
        int busy = 0;

        while ((cred = sh_msg_recv_cred(ctl, &m)) > 0) {
            char in_path[64], out_path[64], n[24], line[256];
            ws_ctl_t done;
            sh_stat_t ist;
            long len;
            uint64_t q, from = (uint64_t)cred & 0xffffffffu, nl = 0;
            int is_raw;
            busy = 1;
            /* A request is honoured only from the process it names. */
            if (m.type != WS_CTL_RUN || m.pid != from) continue;
            req_uid = (uint32_t)((uint64_t)cred >> 32);
            str_copy(in_path, WS_IO_DIR "/", sizeof(in_path));
            str_cat(in_path, fmt_u64(n, m.pid, 10), sizeof(in_path));
            str_copy(out_path, in_path, sizeof(out_path));
            str_cat(in_path, ".in", sizeof(in_path));
            str_cat(out_path, ".out", sizeof(out_path));
            /* The request file must be the requester's own and not
             * writable by others, or someone could swap the command. */
            len = sh_stat(in_path, &ist) == 0 && ist.uid == req_uid && !(ist.mode & 0002)
                      ? sh_read(in_path, (char *)inbuf, sizeof(inbuf) - 1) : -1;
            raw_ok = 0;
            is_raw = 0;
            if (len < 0) {
                out_len = 0;
                say("ws: request refused (the request file is not yours alone)\n");
            } else {
                inbuf[len] = 0;
                while (nl < (uint64_t)len && inbuf[nl]) nl++;
                str_copy(line, (char *)inbuf, sizeof(line));
                req_payload = nl + 1 <= (uint64_t)len ? inbuf + nl + 1 : 0;
                req_payload_len = req_payload ? (uint64_t)len - nl - 1 : 0;
                is_raw = str_starts(line, "raw ");
                run_command(line);
                req_payload = 0;
                mem_wipe(inbuf, (uint64_t)len);
            }
            /* The answer may hold decrypted content: make the file the
             * requester's and private before writing it. */
            (void)sh_write_file(out_path, "", 0);
            (void)sh_chown(out_path, req_uid);
            (void)sh_chmod(out_path, 0600);
            (void)sh_write_file(out_path, out, out_len);
            mem_wipe(out, out_len);
            mem_set(&done, 0, sizeof(done));
            done.type = WS_CTL_DONE;
            done.pid = m.pid;
            done.status = is_raw && !raw_ok;
            str_copy(line, "/proc/", sizeof(line));
            str_cat(line, n, sizeof(line));
            str_cat(line, "/ws", sizeof(line));
            q = sh_msg_open(line);
            if (q) (void)sh_msg_send(q, &done);
        }
        while (chan_poll(&rx)) {
            busy = 1;
            handle_peer(&rx);
        }
        if (st.active && sh_ticks() - last_beat >= BEAT_TICKS) {
            last_beat = sh_ticks();
            heartbeat();
            write_status();
        }
        for (int i = 0; i < MAXM; i++) {
            member_t *mm = &st.m[i];
            int ch;
            if (!mm->used || !mm->need_sync || (ch = chan_find(mm->sign_pub)) < 0) continue;
            mm->need_sync = 0;
            set_busy("Syncing with ", mm->name);
            (void)sync_with(ch);
            set_busy(0, 0);
            files_dirty = 1;
            write_status();
            break; /* one per pass keeps the daemon responsive */
        }
        if (sync_pending) {
            int which = sync_pending;
            sync_pending = 0;
            if (which == 2) {
                notify_all();
            } else {
                for (int i = 0; i < MAXM; i++) {
                    if (st.m[i].used && mem_eq_ct(st.m[i].ip, sync_from, 4)) {
                        int ch = connect_member(&st.m[i]);
                        if (ch >= 0) (void)sync_with(ch);
                    }
                }
            }
        }
        if (!busy) sh_sleep(1);
    }
}
