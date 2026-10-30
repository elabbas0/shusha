# Shusha encryption and decryption

This file covers how data is actually encrypted and decrypted in each layer,
and which key does what. Primitives and policy are in
[THREAT_MODEL.md](THREAT_MODEL.md#crypto-policy); key ownership and
permissions are in [WORKSPACE.md](WORKSPACE.md).

## 1. Layers at a glance

A workspace file, at each point in its life:

```
on a member's disk   [ home-volume layer ( file layer ( content ) ) ]   + signature
on the network       [ secure channel   ( file layer ( content ) ) ]    + signature
in the cloud backup  [                    file layer ( content )   ]    + signature
in a live session    [ secure channel   ( session layer ( op batch ) ) ] + signature
```

| Layer | Protects against | Key | Algorithm |
|---|---|---|---|
| L1 Key store | Stolen disk getting private keys | Password (Argon2id) + 2-of-3 hardware binding | XChaCha20-Poly1305 |
| L2 Home volume | Stolen disk reading local files | Per-user volume key VK | XChaCha20-Poly1305, per-file subkeys |
| L3 Transport | Network snooping, tampering, replay | Channel session keys (ephemeral, hybrid X25519 + ML-KEM) | ChaCha20-Poly1305, counter nonces |
| L4 Workspace file | Other members, storage nodes, cloud, relays | Per-file key FK (per epoch) | XChaCha20-Poly1305, 64 KiB chunks |
| L5 Live session | Same as L4, for co-editing ops | Session key SK = HKDF(FK, session) | XChaCha20-Poly1305 |
| L6 Signatures | Forged or unauthorized edits | Device signing key | Ed25519 |

Each layer stops a different adversary, which is what makes multiple layers
worth having. Breaking one layer still leaves the others intact.

## 2. Shared building blocks

### AEAD (authenticated encryption)

- **Stored data uses XChaCha20-Poly1305 with a random 24-byte nonce.** Many
  writers share one FK, and a 192-bit random nonce makes collisions
  practically impossible without coordinating counters.
- **Transport uses ChaCha20-Poly1305 with a 64-bit counter nonce,** as Noise
  specifies.
- **AAD (authenticated but not encrypted) always binds the context:** format
  version, object id, chunk index, and a last-chunk flag. This means:
  - chunks cannot be swapped between files;
  - chunks cannot be reordered;
  - a file cannot be truncated without detection.
- **Fail closed:** if a tag does not verify, return an error and show
  nothing. Never return plaintext that has not been authenticated, not even
  partially.

### Wrapping a key for a person (public-key wrap)

This is used to give FK, VK, or a rank key to a specific user. It works like
HPKE, in hybrid mode:

```
wrap(recipient_pub, K):
    e            = fresh X25519 key pair
    ss1          = X25519(e.priv, recipient.x25519_pub)
    (ct2, ss2)   = ML-KEM-768.Encaps(recipient.mlkem_pub)
    kek          = HKDF(ss1 || ss2, salt = e.pub || ct2,
                        info = "shusha-wrap-v1" || recipient_key_id || purpose)
    n            = random 24 bytes
    return { recipient_key_id, e.pub, ct2, n, XChaCha20-Poly1305(kek, n, K, aad = purpose) }

unwrap(my_priv, box):
    ss1  = X25519(my_priv.x25519, box.e_pub)
    ss2  = ML-KEM-768.Decaps(my_priv.mlkem, box.ct2)
    kek  = HKDF(... same inputs ...)
    K    = XChaCha20-Poly1305.Open(kek, box.n, box.ct, aad = purpose)   # fails closed
```

- An attacker has to break **both** X25519 and ML-KEM to recover K.
- `purpose` (for example `"fk:<file id>:<epoch>"`) is mixed into both the
  derived key and the AAD, so a wrapped key cannot be replayed into a
  different context.

### Wrapping a key under another key (symmetric wrap)

Used for the rank-key chain and for file keys under a rank key:

```
wrap_sym(KEK, K, purpose) = n || XChaCha20-Poly1305(KEK, n, K, aad = purpose)
```

## 3. L1: key store (`keyd`)

- **What:** the user and device private keys.
- **Encrypt (once, at install or password change):**
  1. `pw_key = Argon2id(password, salt)`
  2. `hw_key = ShamirCombine(2 of 3 shares)`. Each share is decrypted with
     `SHA-256(hardware component)`.
  3. `wrap_key = HKDF(pw_key || hw_key, "shusha-device-v1")`
  4. Store `XChaCha20-Poly1305(wrap_key, private keys)`.
- **Decrypt (at login):** the same steps run in `keyd`. A wrong password or
  wrong hardware means the tag fails, and login fails.
- **After unlock:**
  - Private keys stay inside `keyd`'s memory only. They are never paged to
    disk, never sent over IPC, and wiped at logout or shutdown.
  - Other processes ask `keyd` to *sign*, *unwrap*, or *wrap*. They never get
    raw private keys.

## 4. L2: home volume (local files)

- **Keys:**
  - `VK` = a random 32-byte volume key per user, wrapped under the user's
    public key in L1.
  - Per-file subkey `k_f = HKDF(VK, file_nonce)`, with a random 16-byte
    `file_nonce` in the file header.
- **Encrypt on write:** the file is split into 64 KiB chunks. Each chunk is
  stored as `XChaCha20-Poly1305(k_f, n_i, chunk_i, aad = ver || inode || i || last)`.
  File names are encrypted with `k_name = HKDF(VK, dir_nonce)`.
- **Decrypt on read:** the kernel VFS decrypts chunk by chunk and verifies
  every tag.
- **Where the key lives:** at login, `keyd` unwraps VK and hands it to the
  kernel's per-user key slot. Kernel and `keyd` are the only places VK ever
  exists. Logout wipes the slot.
- **Access control still applies on top.** Another logged-in user is blocked
  by uid/mode checks, *and* their files use a different VK.

## 5. L3: transport (`netd`)

- **Handshake (implemented, `user/lib/chan.c`):** a SIGMA-I style
  exchange over UDP, used instead of Noise because it needs only the
  primitives we already have and keeps identities hidden:
  ```
  HS1  I -> R : sid_i, eI (X25519), ekI (ephemeral ML-KEM-768 key)
  HS2  R -> I : sid_i, sid_r, eR, ct = Encaps(ekI),
                Enc_hs_r( pub_R, Sign_R("shusha-ch-R" || th1) )
  HS3  I -> R : sid_r, Enc_hs_i( pub_I, Sign_I("shusha-ch-I" || th2) )
  prk = HKDF-Extract(th1, X25519(eI, eR) || ML-KEM shared secret)
  ```
  - Hybrid post-quantum: reading a session needs both X25519 and
    ML-KEM-768 broken. Both halves are ephemeral, so it is forward secret.
  - Both sides prove their Ed25519 device key by signing the transcript
    hash; the identities travel encrypted, so a passive observer cannot see
    who is talking.
  - The channel only authenticates a key. `workspaced` then requires that
    key to be a member in the verified log (or, for a join, to present a
    valid invite proof bound to that key).
- **Data:** each frame is ChaCha20-Poly1305 under per-direction keys
  expanded from prk and the final transcript hash, with a 64-bit counter
  nonce. A 64-packet sliding window rejects replays and duplicates.
- **Rekeying (not yet implemented):** every 2^20 messages or 10 minutes,
  plus a fresh handshake every hour.
- **Session keys** never touch disk.

## 6. L4: workspace files

### Object format (version 1)

```
FileObject {
    ver            = 1
    file_id        = 16 random bytes
    epoch          = u32            # bumped on rekey (member removed, grant revoked)
    owner_member   = member id
    lockbox        = [ entries ]    # who can obtain FK
    name_ct        = XChaCha20-Poly1305(FK, name || mime || size)
    chunks         = [ n_i || XChaCha20-Poly1305(FK, n_i, chunk_i,
                         aad = ver || file_id || epoch || i || last) ]
    merkle_root    = SHA-256 Merkle root over (header fields || chunk ciphertexts)
    sig            = Ed25519(author_device_key, merkle_root)
}

lockbox entry  =  { kind = "member", wrap(member_pub, FK) }            # owner, grantees
               |  { kind = "rank",   rank = r-1, wrap_sym(RK_(r-1), FK) }   # everyone above owner
```

### Encrypt (create or save a file)

1. If this file and epoch have no FK yet, generate a random 32-byte FK.
2. Fill the lockbox:
   - an entry for the owner;
   - an entry for `RK_(owner_rank - 1)` (unless the owner is the admin);
   - an entry for each active grantee.
3. Encrypt the name, then each chunk with a fresh random nonce.
4. Build the Merkle root and ask `keyd` to sign it.
5. Hand the object to `workspaced` for replication.

### Decrypt (open a file)

1. **Check before anything else:** verify the signature and the Merkle root,
   and confirm in the log that the author had edit rights at that log
   position. If any check fails, reject the object.
   - These checks need no FK, so storage nodes and non-readers also reject
     forgeries and never replicate them.
2. Find a way to FK:
   - a `member` entry for me → `keyd` unwraps it; or
   - a `rank` entry for rank j < my rank → `keyd` takes my RK and walks down
     the chain (RK_mine opens RK_(mine+1), and so on) until it reaches RK_j,
     then opens FK.
   - No entry reachable = no permission. The UI offers "Request view".
3. Decrypt the name and the chunks. Each tag is verified, and the whole thing
   fails closed.
4. FK is cached in `keyd` while the file is open, then wiped.

### Grants and revocation

- **Grant view:** the owner or a superior adds `wrap(grantee_pub, FK)` to the
  lockbox and writes a `grant` log entry.
- **Grant edit:** a `grant` log entry with edit rights; edits are checked by
  signature. A grantee also needs view, so they get a lockbox entry too.
- **Revoke, or remove a member:**
  - bump the epoch;
  - generate a new FK;
  - re-encrypt (new saves at once, old chunks in the background);
  - drop the revoked lockbox entry.
  The old FK only ever opened data the revoked person already had.

## 7. L5: live co-editing sessions

- `SK = HKDF(FK, info = "shusha-session-v1" || file_id || epoch || session_id)`.
  Everyone with FK can derive it, so no extra key distribution is needed.
- **Op batch (about every 150 ms):**
  ```
  ct  = XChaCha20-Poly1305(SK, n, ops, aad = file_id || session_id || sender || seq)
  sig = Ed25519(sender_device_key, file_id || session_id || sender || seq || n || ct)
  ```
  - **Sign over the ciphertext.** Relays and storage nodes can then verify
    "an authorized editor sent this" without being able to read it.
  - `seq` per sender detects drops and replays.
- **Presence and join requests** are encrypted under SK too, so only readers
  of the file see who is editing.
- **When a session ends,** its ops are folded into a normal L4 save (new
  chunks, signed).

## 8. Workspace-wide keys

| Key | Held by | Wrapped how | Used for |
|---|---|---|---|
| `RK_r` (rank key) | Members at rank r, plus everyone above | `wrap(member_pub)` per member; `wrap_sym(RK_(r-1))` up the chain | Opening FKs of files owned below rank r+1 |
| `WK` (workspace key) | All members, including storage nodes | `wrap(member_pub)` per member | Encrypting the membership log and the file directory for cloud backup; deriving LAN beacons |
| `beacon_key` | All members | `HKDF(WK, "beacon")` | `HMAC(beacon_key, hour)` for LAN and DHT discovery ids |

- **Every RK and WK is rotated on member removal.** New versions are wrapped
  for the remaining members only.

## 9. Randomness

Every key, nonce, and ephemeral value above comes from one source: the kernel
CSPRNG.

- **Seeding:** RDSEED (or RDRAND), CPU timing jitter, and interrupt timing,
  mixed through SHA-256.
- **Output:** a ChaCha20 DRBG, reseeded periodically.
- **Fork safety:** the DRBG state is never copied into child processes.
- **Before seeding:** blocking callers wait until it is seeded, and no key
  generation is allowed. This replaces ICDA's `sched_ticks()`-based values in
  `kernel/net/tls.c`.
- **Interface:** userspace gets random bytes through a getrandom-style IPC
  call to the kernel. `/dev/random` is exposed as a dev node.

## 10. Where the code lives

- **One crypto library** (`crypto/`) is compiled twice: into the kernel and
  into `libshushacrypto` for `keyd`, `netd`, and `workspaced`.
- **The kernel uses it for:** the CSPRNG, L2 (home volume), and verifying
  boot assets later.
- **`keyd` is the only process that holds private keys and unwrapped rank
  keys.**
- **Every primitive has its official test vectors run in CI:**
  - RFC 8439 (ChaCha20-Poly1305)
  - draft-irtf-cfrg-xchacha (XChaCha20-Poly1305)
  - RFC 8032 (Ed25519)
  - RFC 7748 (X25519)
  - FIPS 203 (ML-KEM)
  - RFC 5869 (HKDF)
  - RFC 9106 (Argon2id)
  - channel handshake transcripts (known-answer, tamper and replay cases)
- **Every format parser is fuzzed:** lockbox, FileObject, channel frames, and
  log entries.

## 11. Build order

1. CSPRNG, then the ChaCha20-Poly1305 / XChaCha20-Poly1305 AEAD and HKDF,
   each with test vectors.
2. Ed25519 and ML-KEM-768. Hybrid `wrap()`/`unwrap()`.
3. Argon2id, Shamir 2-of-3, and `keyd` (L1).
4. L2 home volume in the VFS.
5. Secure channel (L3) - done, in `workspaced` for now; moves to `netd`.
6. FileObject, lockbox, and the rank chain (L4).
7. Session keys and signed op batches (L5).
