# Shusha workspace design

A **workspace** is a shared, encrypted space that only invited Shusha devices
can join. It works on a LAN and over the internet, keeps going when the admin
is offline, and supports real-time co-editing with rank-based control.

## 1. Identities and keys

### Device keys (no TPM required)

Every machine generates, at install time, from the CSPRNG:

- an Ed25519 signing key pair (device signing key);
- an X25519 + ML-KEM-768 key pair (device agreement key).

**The key is random. It is not *derived from* the hardware ID.** Hardware IDs
(SMBIOS UUID, NIC MAC, disk serial) are readable by any program, often
printed on a label, and spoofable. A key computed from them would be a key
anyone can recompute. Instead the hardware ID *binds* the random key to the
machine:

```
pw_key   = Argon2id(password, salt, m=256 MiB, t=3)      # salt stored on disk
hw_key   = 2-of-3 threshold over SHA-256(component_i)    # see below
wrap_key = HKDF(pw_key || hw_key, info="shusha-device-v1")
on disk  = ChaCha20-Poly1305(wrap_key, device_private_keys)
```

- **Hardware components:** SMBIOS system UUID, primary NIC MAC, and boot disk
  serial. ICDA's ATA/AHCI drivers already issue IDENTIFY, which carries the
  serial; NVMe Identify does the same.
- **2-of-3 threshold:** `hw_key` is Shamir-split into 3 shares. Share *i* is
  stored encrypted under `SHA-256("shusha-hw" || component_i)`. Any two
  matching components rebuild `hw_key`, so swapping one NIC or disk does not
  lock you out.
- **Effect:** a stolen disk in another machine fails the hardware check. A
  cloned hardware ID without the password fails Argon2id.
- **If a TPM is present:** additionally seal `wrap_key` to it. It is optional,
  never required.
- **Auto-login (no password) is allowed but labelled weak.** In that case
  `pw_key` is empty and only the hardware binding protects the keys.

### User identity keys

- Each local user account has its own Ed25519 + X25519/ML-KEM key set.
- The user key signs a certificate for each device the user logs in from.
  One person, several machines, one identity.
- User keys are wrapped like device keys and unlocked at login by `keyd`.

### Recovery

- At account creation, a 24-word recovery phrase encodes a recovery key that
  also wraps the user key.
- Lose the password and the phrase and the data is gone. That is the price
  of nobody else being able to read it.
- Optional: a user may escrow their recovery key to the workspace admin,
  consistent with "admin has highest authority".

## 2. Workspace, membership, and ranks

### Genesis and the membership log

- Creating a workspace writes a **genesis entry** signed by the creator's user
  key. `workspace_id = SHA-256(genesis)`. The creator is **rank 0 (admin)**.
- All membership changes go into an **append-only, hash-chained log**. Each
  entry contains the previous entry's hash, a type, a body, and the author's
  signature.
- Every member replicates the log and recomputes the state (who is in, ranks,
  grants). An entry is accepted only if its author was authorized *at that
  point in the log*.

Entry types:

| Entry | Who may author it |
|---|---|
| `invite` (invitee key, rank) | Anyone holding the `steward` capability, only for ranks below their own |
| `join` (invite acceptance) | The invitee |
| `set_rank` | A steward, only for members strictly below them, never raising anyone to or above themselves |
| `remove` | A steward, for members strictly below them |
| `grant` / `revoke` (file, view or edit, expiry) | File owner or anyone ranked above the owner |
| `delegate` (give or take `steward`) | Admin |
| `succession` (pre-signed admin handover) | Admin |
| `rekey` | Automatic after `remove`, see §3 |

### Ranks

- Lower number = more authority. Rank 0 is the admin.
- By default each new member gets the next rank in join order (1, 2, 3...).
- The admin (or a steward, for members below the steward) can re-rank people
  later.
- Equal ranks are allowed. Members of equal rank are treated as peers: they
  need permission to read or edit each other's files.

### Joining (LAN or internet)

1. A steward creates an invite: a signed `invite` log entry plus a one-time
   **invite code** (shown as a QR code or text, like `shusha:ws/…`).
2. The invite code carries the workspace id, the steward's address hints, and
   a one-time secret, and is shared out of band.
3. The joining device connects, proves knowledge of the one-time secret
   inside the Noise handshake, receives the log, and appends `join`.
4. Only then do peers accept the device. A modified client without a valid
   invite chain is just an unknown key and gets dropped.

### When the admin is offline

- **Reading and editing keep working without the admin.** Every member holds
  the log and the keys they are entitled to.
- **Membership changes** need a steward. The admin should delegate `steward`
  to one or two trusted high ranks so invites still work.
- **Succession:** the admin pre-signs a `succession` entry that names a deputy
  and a timeout (for example 30 days). If the admin writes nothing to the log
  for that long, the deputy can activate the entry and become rank 0. Until
  then it is inert.

## 3. File encryption and permissions

### Key layers

```
rank key RK_r     one per rank level; RK_r is wrapped for every member at rank r
                  and also wrapped under RK_(r-1), so holding RK_j unlocks all ranks below j
file key FK       random per file (rotated per version epoch)
content           ChaCha20-Poly1305(FK, chunk), chunked at 64 KiB, chunk index in AAD
name / metadata   encrypted under FK as well
```

When a member at rank *r* creates a file, FK is wrapped for:

1. the owner's user key;
2. RK_(r-1), so everyone ranked above the owner can read it (unless r = 0);
3. any explicitly granted member's user key.

The result matches the spec:

- Higher ranks can read and edit lower ranks' files without asking.
- Lower or equal ranks need a `grant` to view, and another to edit.
- A higher rank never needs anything from a lower rank.

### How each permission is enforced

| Permission | Enforced by |
|---|---|
| Read | Cryptography: you either received a wrapped FK or you did not |
| Edit | Signatures: every edit is signed, and peers verify it against the log |
| Membership/rank | Signatures on log entries, checked at their log position |

A patched client can always change its *local* copy. It cannot make honest
peers accept an unauthorized edit or decrypt a file it has no key for.

### Permission requests

- A lower rank presses "Request view" or "Request edit" on a file it can see
  listed (the listing shows encrypted names as "Locked file by <owner>").
- The request goes to the owner and the owner's superiors. The first one to
  answer decides.
- A grant can be one-time (this session), time-limited, or permanent. It is
  written as a `grant` entry with an expiry.
- Requests time out after 24 h by default.

### Removal and rekeying

- `remove` makes every member who holds a rotated key generate new ones:
  - new RK for the removed member's rank and every rank below it;
  - new FK epochs for every file the removed member could read.
- New versions use the new keys immediately. Old chunks are re-encrypted in
  the background.
- The removed member keeps what they already had (see THREAT_MODEL "Limits").

## 4. Real-time co-editing

### Data model

- Each document is a **sequence CRDT** (RGA/Fugue-style list), so concurrent
  edits always merge to the same result with no central server.
  - Op id = (member id, counter).
  - Ops: insert after id, delete id, and later formatting.
- Ops are batched every ~150 ms. Each batch is encrypted under the file's
  live session key and signed with the editor's device key.
  - Session key: `SK = HKDF(FK, session_id)`.
  - Signing every keystroke is wasteful; batches keep the cost negligible.
- Peers verify the signature and the editor's edit right before applying a
  batch. Invalid batches are dropped and logged.
- The CRDT state is periodically compacted into an encrypted snapshot.

### Edit sessions and rank control

A live **edit session** exists while anyone is editing a file.

1. **Session lead:** the highest-ranked participant. Lead passes automatically
   when they leave.
2. **Joining an active session:**
   - If you outrank the lead, you join instantly and become lead.
   - If the lead outranks you (or is your equal), you send a **join request**,
     and the lead must approve it. This is "the higher-priority editor must
     allow the lower one". Your standing edit grant lets you *ask*. It does not
     let you barge in while a superior is editing.
3. **No active session:** you may start one if you have edit rights (owner,
   higher rank, or a valid edit grant).
4. **Lead controls:**
   - approve or deny join requests;
   - downgrade a participant to view-only;
   - remove a participant from the session;
   - lock sections (later).
5. **Presence:** everyone with read access sees a live list of who is viewing
   and who is editing, with their ranks.
   - The list is encrypted under FK, so only readers see it.
   - Cursors and colours per editor are planned.
   - Presence cannot be hidden, not even by the admin (decision D2).
6. **Offline edits:** they merge through the CRDT on reconnect, and the merge
   is still subject to signature and rights checks at the time of each op.

## 5. Networking: LAN and internet, free

### Transport

- Noise XX for the first contact and Noise IK afterwards, with an ML-KEM-768
  encapsulation mixed into the chaining key (hybrid post-quantum).
- It runs over UDP with our own small reliability layer, so that NAT hole
  punching works. TCP is the fallback on networks that block UDP.
- Every frame is authenticated. There are replay counters per session.

### Discovery

- **LAN:** UDP broadcast/multicast beacon carrying
  `HMAC(workspace_secret, current_hour)`. Members recognise it; outsiders see
  random bytes and cannot tell which workspace it is.
- **Internet, no server at all:** publish a signed, encrypted "where am I"
  record into the public BitTorrent mainline DHT, keyed by the same blinded,
  time-rotating id. It is free, global, and has no account.
- **Internet, more reliable:** an optional tiny rendezvous/relay service. It
  sees only ciphertext and blinded ids, so it is untrusted (adversary A6).
  It can run on:
  - any member's machine with a public IP or a forwarded port;
  - a free-tier cloud VM (verify the provider's current always-free terms
    before relying on one).
- **NAT traversal:** public STUN servers (free) reveal each peer's public
  address, then peers hole-punch.
  - If both sides are behind symmetric NAT, traffic goes through a relay
    (another member or the rendezvous box). Relays are end-to-end encrypted.

### "Cloud for free": the members are the cloud

- **Every member stores the workspace ciphertext**, including files they
  cannot read. They cannot decrypt it, but they can serve it. As long as *one*
  member or storage node is online, everyone can sync.
- **Storage nodes:** any always-on machine (an old PC running Shusha) can join
  as a non-human member with rank "storage". It holds ciphertext and the log,
  has no read keys, and can relay.
- **Optional cloud backup:** encrypted chunks can be pushed to any free object
  storage tier as an extra replica. It only ever sees ciphertext, so the
  provider does not need to be trusted.
- **Scaling later:** Reed–Solomon erasure coding (k of n), so each member
  stores a fraction instead of everything.
- **What it cannot do:** real-time co-editing needs the co-editors to be
  online together, either directly or through a relay. Everything else
  syncs when anyone comes back.
