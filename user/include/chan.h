#ifndef SHUSHA_CHAN_H
#define SHUSHA_CHAN_H

/* Secure channel between Shusha machines over UDP (docs/CRYPTO.md, L3).
 *
 * SIGMA-I style authenticated key exchange, hybrid post-quantum:
 *   HS1  I -> R : sid_i, eI (X25519), ekI (ML-KEM-768 ephemeral)
 *   HS2  R -> I : sid_i, sid_r, eR, ct = Encaps(ekI),
 *                 Enc_hs_r(pub_R, Sign_R("shusha-ch-R" || th1))
 *   HS3  I -> R : sid_r, Enc_hs_i(pub_I, Sign_I("shusha-ch-I" || th2))
 * prk = HKDF-Extract(th1, X25519(eI,eR) || ML-KEM shared secret); every
 * key is expanded from prk and the transcript hash, so the session is
 * forward secret, needs both X25519 and ML-KEM broken to be read, and
 * both device identities (Ed25519 keys) are authenticated and hidden
 * from passive observers.  Data packets: ChaCha20-Poly1305, 64-bit
 * per-direction counters, 64-packet replay window. */

#include <stdint.h>
#include "net.h"

#define CHAN_PORT 7100
#define CHAN_MAX 16
#define CHAN_MAX_MSG 1360

typedef struct {
    int chan;                 /* channel index */
    uint8_t peer_pub[32];     /* authenticated Ed25519 identity of the peer */
    net_addr_t from;
    uint16_t len;
    uint8_t data[CHAN_MAX_MSG];
} chan_msg_t;

/* Device identity used to authenticate this end. */
int  chan_init(const uint8_t sign_seed[32], const uint8_t sign_pub[32], uint16_t port);
/* Open (or reuse) an authenticated channel to ip:port.  Pumps incoming
 * traffic while waiting; messages that arrive meanwhile are queued for
 * chan_poll.  Returns channel index or -1. */
int  chan_connect(const uint8_t ip[4], uint16_t port, uint64_t timeout_ticks);
/* Non-blocking: the open channel to ip:port, or -1 after starting (or
 * re-sending) a handshake that chan_poll will complete. */
int  chan_open(const uint8_t ip[4], uint16_t port);
/* The open channel authenticated as peer_pub, or -1. */
int  chan_find(const uint8_t peer_pub[32]);
int  chan_send(int chan, const void *data, uint64_t len);
/* Next decrypted application message (1), or 0 if none waiting. */
int  chan_poll(chan_msg_t *out);
const uint8_t *chan_peer(int chan);
void chan_close(int chan);
/* Fill buf from /dev/random (dies loudly if unavailable). */
void rand_bytes(void *buf, uint64_t len);

#endif
