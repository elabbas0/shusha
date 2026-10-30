# Shusha plan

Shusha is a security-first, multi-user, networked OS built on the ICDA kernel
and drivers (plus ICDA's window manager). Its flagship feature is the
**Workspace**: an encrypted shared space with ranked permissions and
real-time co-editing, working over LAN and internet with no paid servers.

- Threat model: [THREAT_MODEL.md](THREAT_MODEL.md)
- Workspace protocol: [WORKSPACE.md](WORKSPACE.md)
- Encryption and decryption flows: [CRYPTO.md](CRYPTO.md)

## Requirements (from the project owner)

| # | Requirement | Design answer |
|---|---|---|
| R1 | Encryption nobody else, including strong AI, can break | Standard primitives, own protocol and key hierarchy (THREAT_MODEL "Crypto policy") |
| R2 | Multiple encryption layers | Disk, device, transport, file, and signed-edit layers, each stopping a different adversary |
| R3 | Only Shusha devices can connect to a workspace | Cryptographic invite chain; software identity is never trusted |
| R4 | First creator is admin; later joiners get lower ranks | Genesis entry = rank 0; next rank by join order; re-rankable |
| R5 | Lower ranks need permission to view or edit higher ranks' files; never the reverse | Rank-key chain plus signed grants (WORKSPACE §3) |
| R6 | Admin has the highest authority | Admin holds RK_0, which unlocks everything; only the admin delegates stewards |
| R7 | LAN **and** internet | Noise over UDP, LAN beacons, DHT/rendezvous, STUN + relays (WORKSPACE §5) |
| R8 | Works with the admin offline, cloud-like, free | Members and storage nodes replicate ciphertext; stewards; pre-signed succession |
| R9 | Real-time co-editing; a higher editor must allow a lower one in; everyone sees who is editing | CRDT + edit sessions with a rank-based lead and visible presence (WORKSPACE §4) |
| R10 | No TPM required; key tied to hardware ID | Random key, hardware ID *binds* it (2-of-3) together with the password; TPM used if present |

## Architecture

```
┌──────────────────────── userspace ────────────────────────┐
│ Workspace app (WM)   Login screen   Files   Settings       │
│        │                  │                                │
│   workspaced ─────── keyd ─────── authd                    │
│   (log, CRDT,        (key store,  (accounts,               │
│    sync, presence)    wrap/unwrap) Argon2id)               │
│        │                  │                                │
│   netd (Noise, UDP reliability, discovery, NAT, relay)     │
├──────────────── IPC: SYS_MSG_* / SYS_SHM_* ────────────────┤
│ ICDA kernel: enforced uid/permissions, CSPRNG, crypto      │
│ primitives, UDP/TCP listen, per-process isolation          │
│ drivers: e1000, virtio-net, AHCI/ATA/NVMe, fb/virtio-gpu   │
└────────────────────────────────────────────────────────────┘
```

- The ICDA native syscall ABI is frozen, so new services are userspace
  daemons behind the existing IPC calls.
- The kernel only gains what must be in the kernel: randomness, enforcement,
  and network sockets.
- **Key isolation:** `keyd` is the only process that ever holds unwrapped
  private keys. Apps ask it to sign, decrypt, and wrap; they never receive raw
  keys.

## Known blockers in the ICDA base

| Blocker | Where | Fix |
|---|---|---|
| TLS private key and randoms derived from `sched_ticks()`: predictable | `ICDA/kernel/net/tls.c:879-899` | Kernel CSPRNG; all key generation goes through it |
| Process identity inert; `ex_token = sched_ticks()` | `ICDA/kernel/proc/process.h:59`, `user.c:655` | Enforce uid at every gate; random tokens |
| VFS has no owner/mode | `ICDA/kernel/fs/vfs.h` | Add owner uid, mode bits, check in every VFS syscall |
| Missing primitives | `ICDA/kernel/crypto/` | Ed25519, HKDF, ChaCha20-Poly1305, Argon2id, ML-KEM-768 |
| Not constant-time verified | `bn.c`, `rsa.c` | Audit or drop (RSA not needed by Shusha) |
| Net stack is client-only (connect, UDP send) | `ICDA/kernel/net/net.c` | Listening sockets, UDP receive, broadcast/multicast |
| License forbids marketing a derivative as a different product | `ICDA/LICENSE` | Owner adds an explicit Shusha exception |

## Roadmap

Each phase has an exit test that must pass in QEMU before the next one starts.

| Phase | Work | Exit test |
|---|---|---|
| 0. Groundwork | Import ICDA kernel, drivers, and WM into this repo with credit; add the license exception; CI with test vectors | Shusha boots in QEMU; CI green |
| 1. Kernel security | CSPRNG; uid enforcement; VFS owner/mode; new primitives; constant-time audit; UDP receive and listening TCP | RFC/FIPS test vectors pass; user B cannot read user A's file; two VMs exchange UDP |
| 2. Local multi-user | `authd` (accounts, Argon2id), `keyd` (hardware binding, wrapping), login screen, encrypted home folders, recovery phrase | Disk moved to another VM will not unlock; wrong password fails; one swapped component still unlocks |
| 3. Secure transport | `netd` with hybrid Noise over UDP, reliability layer, LAN beacons | Two VMs establish a session; tampered or replayed packets rejected |
| 4. Workspace core | Genesis, log, invites (QR/text), join, ranks, stewards, succession | 5 VMs join in order with ranks 0–4; a patched client without an invite is dropped |
| 5. Encrypted files | Rank keys, file keys, grants and requests, removal and rekeying, ciphertext replication | Rank 3 cannot open rank 1's file until granted; admin opens everything; removed member cannot read new versions |
| 6. Co-editing | CRDT, signed op batches, edit sessions, lead approval, presence UI | Admin and rank 2 co-edit live; rank 3 must be approved by the lead; all readers see the editor list |
| 7. Internet | STUN, hole punching, DHT records, relays, optional rendezvous, storage nodes, cloud backup | Two VMs behind separate NATs sync; workspace syncs with the admin powered off |
| 8. Hardening | Fuzz every parser (Noise, log, CRDT, VFS syscalls); SMEP/SMAP/W^X/ASLR; external review | No crashes after a set fuzzing budget; findings fixed |

## Progress

| Phase | State |
|---|---|
| 0. Groundwork | Done. Kernel/drivers imported from ICDA (only the kernel; userspace is new). New build, initramfs manifest, init, shell. |
| 1. Kernel security | Partly done. CSPRNG + `/dev/random`; TLS/session-token randomness fixed; crypto library complete for the design: ChaCha20-Poly1305, XChaCha20-Poly1305, SHA-2, SHA-3/SHAKE, BLAKE2b, HMAC, HKDF, Argon2id, Ed25519, X25519, ML-KEM-768 and the hybrid key wrap, 245 test-vector checks. Still to do: uid enforcement, VFS owner/mode, listening sockets. |
| UI | New compositor (`wm`), client library, Explorer and Terminal ported from ICDA. |

### Gaps found while building

- **Shared memory and message queues have no access control.** Any process can map any SHM handle or open any named queue, so one app can read another window's pixels. Fix in phase 1 with per-process handle tables (owner + grantee), before multi-user work.
- **Message queues were never freed** (close is a stub in the frozen ABI). Shusha now reclaims queues whose creating process has exited.
- **ICDA's USB stack is missing upstream.** `kernel.c` referenced `drivers/usb/xhci.*` and `memory/dma.*`, which were never committed to ICDA; USB init is removed until those sources exist.
- **Display mode** follows whatever GRUB sets (800x600 in QEMU), by ICDA's design for monitor compatibility.

## Decisions

| # | Decision | Status |
|---|---|---|
| D1 | Admin can read every file | **Decided: yes** |
| D2 | Admin/higher ranks can edit invisibly | **Proposed: no.** Presence is always shown |
| D3 | Equal ranks allowed (peers need permission from each other) | Proposed: yes |
| D4 | Default succession timeout | Proposed: 30 days, admin-configurable |
| D5 | Recovery-key escrow to admin | Proposed: optional, off by default |
| D6 | First internet discovery path: DHT, rendezvous, or both | Proposed: both, DHT first |
| D7 | Document types for co-editing in v1 | Proposed: plain/rich text only |
| D8 | License exception wording for Shusha | Open: owner |
