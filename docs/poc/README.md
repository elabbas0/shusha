# Workspace proof of concept (LAN)

Three unmodified Shusha desktops boot in QEMU on one virtual Ethernet
segment. `scripts/ws-gui-demo.py` drives the **Workspace app** and the
**live top bar** with the mouse and keyboard. Every screenshot below is a
real `screendump` of a running machine; nothing is mocked.

| Machine | Address | Who | Rank |
|---|---|---|---|
| vm1 | 169.254.0.1 | admin (creator) | 0 |
| vm2 | 169.254.0.2 | alice (joined first) | 1 |
| vm3 | 169.254.0.3 | bob (joined second) | 2 |

![All fifteen steps](sheet.png)

## The steps

1. [The Workspace app before anything exists](01-vm1-workspace-app-before.png).
   It offers two paths, create or join. It also states the rule: personal
   files stay on the machine, and nothing enters the workspace unless you
   add it.
2. [vm1 creates `shusha-team`](02-vm1-admin-created.png) and is rank 0.
   The top bar starts showing the workspace live.
3. [A single-use invite code](03-vm1-invite-code.png).
4. [Alice joins from the app](04-vm2-alice-joined.png) and gets rank 1.
   Bob joins next and gets rank 2.
5. [Members with live presence](05-vm1-members-online.png): 3 of 3 online.
6. [Alice adds one of her own files](06-vm2-alice-shared-a-file.png).
   The app reads it with her permissions, then encrypts, signs and
   replicates it.
7. [The admin opens it](07-vm1-admin-opens-lower-rank-file.png). A higher
   rank never needs to ask.
8. [Bob sees only a locked file](08-vm3-bob-sees-a-locked-file.png). Even
   the name is hidden from him. He clicks **Request access**.
9. [The request arrives live in Alice's top bar](09-vm2-request-arrives-in-top-bar.png):
   "bob asks for budget.txt", with a red **1 request** badge.
10. [Alice's Requests view](10-vm2-requests.png): she clicks **Grant**.
11. [Bob's top bar](11-vm3-top-bar-access-granted.png): "You can now open
    budget.txt".
12. [Bob opens it and saves a copy](12-vm3-bob-opens-and-saves-a-copy.png).
    The copy is private (0600) from the start.
13. [vm1 is powered off](13-vm2-admin-offline.png). The admin shows as
    offline and the top bar reads "2 of 3 online". Everyone else carries
    on.
14. [Another user on Bob's machine](14-vm3-other-user-terminal.png) (uid
    1000) gets "This machine's workspace belongs to another user". They
    cannot read Bob's saved copy either.
15. [The app shows them nothing](15-vm3-other-user-app.png).

## What is on the wire

vm3 recorded every packet on the segment during the demo
([full output](wire-check.txt)):

```
secure-channel packets on UDP 7100: 977, between 6 address pairs
  HS1  initiator: X25519 + ML-KEM-768 public keys                    x53
  HS2  responder: X25519 + ML-KEM ciphertext + encrypted identity    x3
  HS3  initiator: encrypted identity + signature                     x5
  DATA ChaCha20-Poly1305 frame                                       x916

encrypted payload: 37469 bytes, entropy 7.993 bits/byte (8.0 = indistinguishable from random)
plaintext '42000 AZN'      not present anywhere in the capture
plaintext 'budget.txt'     not present anywhere in the capture
plaintext 'Q3 budget'      not present anywhere in the capture
plaintext 'alice'          not present anywhere in the capture
plaintext 'bob'            not present anywhere in the capture
plaintext 'shusha-team'    not present anywhere in the capture
```

- **DATA frames** are mostly presence pings, every 2 s per member.
- **The many HS1 packets** are retries towards the powered-off admin.

## Separation from personal files

- **One owner per machine.** The workspace belongs to the local user who
  created or joined it. The kernel stamps each request with the sender's
  uid, and every other user is refused.
- **Adding happens on the user's side.** The app or `ws` reads the file
  with the user's own permissions, and the service never opens a personal
  path. So nobody can push someone else's file into the workspace.
- **Private by default.** User-created files are 0600 and folders 0700.
  Saved copies are 0600.

## Automated checks

`scripts/ws-lan-test.py` runs five machines with 38 assertions. The
[latest run](ws-lan-test.txt) passed all of them. They cover:

- ranks 0–4 in join order;
- forged and reused invites refused;
- all five machines agree on the log;
- rank-based access, request and grant;
- reading and sharing with the admin offline;
- separation:
  - uid 1000 cannot share a root-only file;
  - `get` saves a local copy;
  - another local user is refused.

## Reproduce

```bash
make SERIAL_MIRROR=1 shusha.iso lan.iso
python3 scripts/ws-gui-demo.py              # 15 screenshots -> build/wsshots
python3 scripts/wire-check.py build/lan/vm3.pcap "42000 AZN" "budget.txt"
python3 scripts/ws-lan-test.py              # 5 machines, 38 checks
```

## Limits of this proof

- **LAN only.**
- **One workspace per machine.**
- **State lives in RAM** on the live image.
- **No login screen yet.** The desktop runs as root; other users appear
  through `su`.
- **Not built yet:**
  - removing members and rekeying;
  - stewards;
  - separate edit rights;
  - live co-editing.
- **No outside review yet** of the cryptography and code.
