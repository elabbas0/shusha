# Shusha threat model

Every security decision in Shusha should trace back to a line in this file.
If a feature cannot say which threat it stops, it is not a security feature.

## Assets

| Asset | Where it lives |
|---|---|
| Workspace file contents (and names, sizes) | Member machines, storage nodes, optional cloud backup |
| Live co-editing operations | In transit between members |
| User identity keys | Each user's machine(s), wrapped at rest |
| Device keys | Each machine, wrapped at rest, bound to hardware |
| Membership log (who is in, at what rank) | Replicated on every member |
| Local user data (home folders) | Each machine's disk |

## Adversaries

| # | Adversary | Capabilities | In scope |
|---|---|---|---|
| A1 | Network attacker | Reads, drops, replays, injects packets on LAN or internet | Yes |
| A2 | Outsider with a patched "Shusha" | Runs modified Shusha code, speaks our protocol | Yes |
| A3 | Malicious member | Valid keys at some rank, runs a modified client | Yes |
| A4 | Removed member | Kept every key and file they had before removal | Partially, see "Limits" |
| A5 | Thief | Has the powered-off disk or whole machine | Yes |
| A6 | Untrusted storage / relay | Stores or forwards our ciphertext (free cloud tiers, relays, DHT) | Yes |
| A7 | AI-assisted attacker | Any of the above, plus fast automated bug-finding in our code | Yes |
| A8 | Malware with kernel access on a member's machine | Reads memory, keys, screen | No, a compromised endpoint is game over |
| A9 | Physical attacker with a running, unlocked machine | Cold boot, DMA, hardware implants | No for v1 |

A7 is the one people usually mean by "even AI can't decrypt it". Such an
attacker does not break AES or X25519. It finds a missed bounds check, a
predictable random number, a missing signature check, or a protocol state
mix-up. The defence is a small trusted codebase, standard primitives, test
vectors, fuzzing, and review. Custom ciphers would make us weaker against A7.

## Security properties we promise

1. **Confidentiality:** only members holding a file's key can read it. That
   means the owner, everyone ranked above the owner, and anyone the owner or
   a higher rank explicitly granted. Storage nodes, relays, and the network
   see ciphertext only. File names are encrypted too.
2. **Integrity and authorization:** every change (file edits, grants,
   membership changes) is signed. Honest peers reject anything not signed by
   someone authorized at that point in the membership log. A patched client
   (A2, A3) can make its own copy say anything, but it cannot get honest peers
   to accept it.
3. **Authenticity of membership:** you are a member only if a signed invite
   chain leads back to the workspace genesis entry. "Is it running Shusha?"
   is never an access check.
4. **Forward secrecy for transport:** the network sessions use ephemeral
   keys. A device key leaked later does not decrypt recorded traffic.
5. **Post-quantum hedge:** key exchange is hybrid (X25519 + ML-KEM-768), so
   "record now, decrypt later" needs both to fall.
6. **At-rest protection:** a powered-off disk (A5) reveals nothing without the
   user's password. The hardware binding means the disk alone, moved to
   another machine, is not enough either.
7. **Accountability:** the membership and grant log is append-only and
   hash-chained. Members can see that history was rewritten, even when the
   admin did it.
8. **Presence honesty:** every reader of a file can see who is editing it.
   The admin cannot edit invisibly (decision D2 in PLAN.md).

## Limits (honest list)

- **What was seen cannot be unseen.** A removed member (A4) keeps everything
  they could already read. Revocation protects only *future* versions.
- **A higher rank can read everything below it.** The admin reads everything,
  by design.
- **A compromised endpoint (A8) leaks everything that user can access.** No
  encryption helps once malware owns the kernel.
- **Traffic analysis:** timing and volume between members is visible to A1/A6.
  We blind identifiers but do not pad traffic in v1.
- **Hardware IDs are not secrets.** They bind a key to a machine; they never
  replace the password (see WORKSPACE.md "Device keys").
- **Availability is best effort.** If every member and storage node is
  offline, the workspace is unreachable. We guarantee only that it is never
  *readable* by anyone else.

## Crypto policy

- **No home-made primitives.** Only these, each checked against its official
  test vectors in CI:

  | Use | Primitive | Reference |
  |---|---|---|
  | Signatures | Ed25519 | RFC 8032 |
  | Key agreement | X25519 | RFC 7748 |
  | Post-quantum key agreement | ML-KEM-768 | FIPS 203 |
  | Encryption | ChaCha20-Poly1305 (default), AES-256-GCM | RFC 8439, SP 800-38D |
  | Key derivation | HKDF-SHA-256 | RFC 5869 |
  | Hashing | SHA-256 (BLAKE2s optional for Noise) | FIPS 180-4 |
  | Password hashing | Argon2id | RFC 9106 |
  | Random numbers | ChaCha20 DRBG seeded from RDSEED/RDRAND + jitter | — |
  | Transport handshake | Noise XX / IK with a hybrid ML-KEM step | noiseprotocol.org |

- "Our own encryption" means **our own protocol and key hierarchy**, never our
  own cipher.
- All secret-dependent code runs in constant time (no branches or table
  lookups on secrets). `bn.c`/`rsa.c` from ICDA get audited or dropped.
- Every wire and disk format starts with a version byte, so algorithms can be
  replaced without breaking old data.
- Secrets are wiped from memory after use and never logged. ICDA's serial
  identity logging must never print key material.
