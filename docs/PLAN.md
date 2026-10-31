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
| R7 | LAN **and** internet | Hybrid secure channel over UDP, LAN beacons, DHT/rendezvous, STUN + relays (WORKSPACE §5) |
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
│   netd (channel, UDP reliability, discovery, NAT, relay)   │
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
| 3. Secure transport | `netd` with the hybrid channel over UDP, reliability layer, LAN beacons | Two VMs establish a session; tampered or replayed packets rejected |
| 4. Workspace core | Genesis, log, invites (QR/text), join, ranks, stewards, succession | 5 VMs join in order with ranks 0–4; a patched client without an invite is dropped |
| 5. Encrypted files | Rank keys, file keys, grants and requests, removal and rekeying, ciphertext replication | Rank 3 cannot open rank 1's file until granted; admin opens everything; removed member cannot read new versions |
| 6. Co-editing | CRDT, signed op batches, edit sessions, lead approval, presence UI | Admin and rank 2 co-edit live; rank 3 must be approved by the lead; all readers see the editor list |
| 7. Internet | STUN, hole punching, DHT records, relays, optional rendezvous, storage nodes, cloud backup | Two VMs behind separate NATs sync; workspace syncs with the admin powered off |
| 8. Hardening | Fuzz every parser (channel, log, CRDT, VFS syscalls); SMEP/SMAP/W^X/ASLR; external review | No crashes after a set fuzzing budget; findings fixed |

## Progress

| Phase | State |
|---|---|
| 0. Groundwork | Done. Kernel/drivers imported from ICDA (only the kernel; userspace is new). New build, initramfs manifest, init, shell. |
| 1. Kernel security | Partly done. CSPRNG + `/dev/random`; TLS/session-token randomness fixed; crypto library complete for the design: ChaCha20-Poly1305, XChaCha20-Poly1305, SHA-2, SHA-3/SHAKE, BLAKE2b, HMAC, HKDF, Argon2id, Ed25519, X25519, ML-KEM-768 and the hybrid key wrap, 245 test-vector checks. Files and directories have owners and rwx modes enforced by the file syscalls; `/dev/setuid`, `/dev/chmod`, `/dev/chown`; `fscheck` (21 checks) in the image. Still to do: listening sockets. |
| 3. Secure transport | Done for LAN. Kernel UDP for userspace (`/dev/udp`, `/dev/netinfo`), ARP replies, link-local addressing; SIGMA-I channel with X25519 + ML-KEM-768 and Ed25519 identities, AEAD frames, replay window. Multi-VM harness `scripts/lan.py`. Still to do: reliability layer, rekeying, beacons. |
| 4. Workspace core | v1 done on LAN: genesis, admin-signed hash-chained log, single-use invites, join in order (ranks 0-4 in the 5-VM test), forged/reused invites refused. Stewards and succession to do. |
| 5. Encrypted files | v1 done on LAN: rank-key chain, per-file keys, owner-signed replicated objects, request/grant, reads with the admin offline, workspace bound to one local user, personal files only enter when that user adds them (`scripts/ws-lan-test.py`, 38 checks). Removal and rekeying to do. |
| Workspace GUI | Workspace app, live top-bar status (presence, events, sync progress, request badge), dock entry; `scripts/ws-gui-demo.py` drives it on three desktops. |
| 2. Local multi-user | Done except keyd/encrypted homes: authd (Argon2id + pepper, lockout, recovery keys, admin re-auth), unprivileged login/lock screen, WM sessions, text login, Accounts app, private homes and files by default, privileged syscalls gated (docs/ACCOUNTS.md, `scripts/auth-test.py`, 26 checks + seccheck). |
| Editor | Code: VS Code-style editor with language recognition, syntax colouring, Emmet, suggestions, palette, find/replace, minimap; edits encrypted workspace files in place with versioning (docs/EDITOR.md, `make editor-test`, 43 checks; workspace editing in `ws-lan-test.py`, 44 checks). |
| UI | New compositor (`wm`), client library, Explorer and Terminal ported from ICDA. |

### Gaps found while building

- **Shared memory and message queues had no access control** (any process could map any window buffer or read any queue). Fixed: SHM handles are unguessable capabilities, only a queue's owner can receive from it, `/proc/<pid>/` queue names are reserved for that pid, and messages carry a kernel-stamped sender pid that the WM and apps check. `ipccheck` (in the image) tries these attacks.
- **Message queues were never freed** (close is a stub in the frozen ABI). Shusha now reclaims queues whose creating process has exited.
- **ICDA's USB stack is missing upstream.** `kernel.c` referenced `drivers/usb/xhci.*` and `memory/dma.*`, which were never committed to ICDA; USB init is removed until those sources exist.
- **Ownership limits (for now):** owners and modes are not yet saved by the on-disk persistence layer (live sessions only); the Linux-personality fd syscalls are not permission-checked yet. (Program start is now checked for read permission, and the desktop runs as the signed-in user.)
- **Exception and IRQ returns zeroed the user's RCX.** ICDA's `isr_common`/`irq_common` reloaded the data segments through `cx` after restoring the registers, so any page fault taken from user mode (e.g. stack growth) silently cleared RCX. Fixed by choosing the segments before the pops.
- **Stack growth refused GCC's stack-clash probes.** `orq $0,(%rsp)` faults as a read, and the handler only grew the stack on writes and never above RSP. It now accepts any not-present access in the stack region at or near RSP.
- **New files were world-readable** (0644 by default) in a shared `/home`. Files and folders created by user processes now start private (0600/0700); boot-time and system-image nodes keep 0644/0755.
- **A denied `read_at` returned 0**, indistinguishable from an empty file (the first separation test caught `ws put` of a root-only file "sharing" 0 bytes). It now returns -1.
- **Most machine-wide syscalls were open to every process** (kill any program, format disks, power off, map the framebuffer, read raw keystrokes). They are now root-only (kill: own processes only; input: not while the desktop is active).
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
