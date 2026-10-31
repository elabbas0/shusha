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
3. The joining device opens a secure channel, proves knowledge of the
   one-time secret bound to its authenticated device key, receives the log,
   and the admin appends `join`.
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
   - v1 seals under RK_r instead, because the owner cannot derive RK_(r-1).
     That is equivalent while every rank has one member. Equal ranks (D3)
     need a public-key wrap to RK_(r-1) here.
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

- A SIGMA-I style handshake with ephemeral X25519 and ML-KEM-768 (hybrid
  post-quantum) and Ed25519 device signatures; see CRYPTO.md §5.
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

## 6. What v1 implements (LAN)

`user/sbin/workspaced.c` (the service), `user/apps/ws.c` (the command) and
`user/lib/chan.c` (the secure channel). Tested end to end by
`scripts/ws-lan-test.py`, which boots five QEMU machines on one virtual
Ethernet segment:

| Check | Result |
|---|---|
| vm1 creates the workspace; vm2-vm5 join in order | ranks 0, 1, 2, 3, 4 |
| Forged invite; reused invite | both refused by the admin |
| Every machine replays and verifies the signed log | all five list ranks 0-4 |
| Rank 2 shares a file | ranks 0, 1, 2 read it; ranks 3, 4 see only "locked file" |
| Rank 3 requests access, rank 2 grants | rank 3 reads it, rank 4 is still locked |
| Admin powered off | rank 1 still reads its replica; a new rank-4 file reaches and is readable by ranks 1-3 |

Commands: `ws status | create <name> | invite | join <code> [name] |
members | put <file> | get <file> <dest> | files | cat <file> |
request <file> | requests | grant <file> <rank> | deny <file> <rank> |
sync`.

### The workspace is separate from personal files

- **One owner per machine.** The workspace belongs to the local user who
  created or joined it. The kernel stamps each request with the sender's
  uid, and workspaced refuses everyone else on that machine. They cannot
  see its members, files or status.
- **Nothing enters it unless the user adds it.** `put` (and "Add file" in
  the app) is done by the client, running as the user. It reads the file
  with the user's own permissions and hands over only the contents. The
  root daemon never opens a personal path, so it cannot be used to share
  someone else's file.
- **Copies out are the user's.** `get` / "Save copy" writes with the
  user's own permissions, and the copy is private (0600) from the start.
- **Personal files are private by default.** Files and folders a user
  creates start at 0600 / 0700. The system image stays world-readable.
- **Private plumbing.**
  - Request files must be the requester's own and not writable by
    others.
  - Answers (which can hold decrypted content) are chowned to the
    requester and set 0600 before they are written.
  - The top-bar status file is 0600, owned by the workspace's user.

### Live status

- **Presence.** Members ping each other every 2 s over the secure
  channels. Anyone silent for 7 s is offline, and their channel is
  dropped. When a member comes back, the two sides sync automatically.
- **Events.** workspaced keeps the last 12 events (joins, shares,
  requests, grants, presence) and writes a summary to `/var/ws/status`.
- **Top bar.** The WM shows a live pill with:
  - the workspace, your rank, and who is online;
  - the latest event for 6 s;
  - what it is syncing, while it syncs;
  - a red badge for requests waiting on you.

  Clicking it opens the Workspace app.
- **The Workspace app** (`user/apps/workspace.c`) has these views:
  create/join, overview, members, files (open, save a copy, request
  access, add a file), requests (grant/deny) and activity. It refreshes
  every 2.5 s.

Not yet in v1: stewards and succession, re-ranking, removal and rekeying,
edit grants (any reader holding FK could re-encrypt; objects are only
accepted if signed by the owner or a higher rank), rollback protection for
objects, LAN beacons (members are found by the addresses in the log), the
internet path (section 5), and co-editing (section 4). State lives in
RAM on live images until on-disk ownership persistence lands.
