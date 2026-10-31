# Proof of concept: accounts, the Workspace and Qələm

Three unmodified Shusha desktops boot in QEMU on one virtual Ethernet
segment. `scripts/ws-gui-demo.py` drives them with the mouse and
keyboard, from the first boot to a second user signing in. Every image
below is a real `screendump` of a running machine; nothing is mocked.

| Machine | Address | People | Workspace rank |
|---|---|---|---|
| vm1 | 169.254.0.1 | admin (created the workspace), then guest | 0 |
| vm2 | 169.254.0.2 | alice (joined first) | 1 |
| vm3 | 169.254.0.3 | bob (joined second) | 2 |

![All seventeen steps](sheet.png)

## The steps

### Accounts

1. [First boot](01-first-boot-setup.png). There are no accounts yet, so the
   login screen asks for the administrator. The password is stored only as
   an Argon2id hash under a machine key.
2. [The recovery key](02-recovery-key.png) is shown once.
3. [The user menu](03-user-menu.png): Accounts, Lock, Log out, Restart,
   Power off.
4. [The admin adds a local user](04-admin-adds-a-local-user.png),
   "guest". Every change asks for the admin's own password again, and the
   new account gets its own recovery key.

### The Workspace

5. [Alice joins from the Workspace app](05-alice-joined-from-the-app.png)
   with a single-use invite code and becomes rank 1. Bob joins next as
   rank 2. The top bar shows the workspace live: rank, and 3 of 3 online.

### Qələm, the editor

6. [Emmet in a new file](06-qalam-emmet-suggestion.png). `!` and Tab give
   the HTML5 page. `main.page>h1{Shusha Workspace}+ul>li.item$*3` shows its
   expansion before Tab is pressed.
7. [Saved as an encrypted workspace file](07-saved-as-encrypted-workspace-file.png).
   The library now lists `index.html`, owned by Alice (avatar "A"). The
   footer capsule reads "encrypted · shusha-team".
8. [The admin edits it](08-admin-edits-and-saves.png). A higher rank may
   edit a lower rank's file. Saving re-encrypts it as version 2, signed by
   the admin, and replicates it to both members.
9. [The edit arrives at Alice](09-alice-sees-the-edit-arrive.png). Her top
   bar says "admin edited index.html".
10. [Bob sees a locked file](10-bob-sees-a-locked-file.png). Rank 2 cannot
    open a rank 1 file: the library shows a padlock, and even the name is
    hidden. He requests access in the Workspace app, and Alice grants it.
11. [Bob reads it, read-only](11-bob-read-only-after-grant.png). A grant
    lets a lower rank read, not edit. The outline lists `main`, the
    heading and `footer`.
12. [CSS with Emmet](12-css-emmet.png): `df`, `jcsb`, `p12-20`, `bdrs8`,
    `c#fc0` and `bgc#1e1e1e`, each followed by Tab, become six
    declarations in `.card`.
13. [Python](13-python-outline-and-suggestions.png): colouring, the
    outline (`greet`), and a suggestion of a word from the file.
14. [The finder](14-finder.png) (F1): actions and files, with their keys.

### Sessions

15. [The lock screen](15-lock-screen.png). While locked, every other window
    is hidden from view and gets no input.
16. [Sign-out, then the sign-in screen](16-sign-in-screen-two-users.png)
    with both local users.
17. [Guest signs in and opens the Workspace app](17-guest-sees-no-workspace.png):
    "This machine's workspace belongs to another user." Guest sees none of
    its members or files.

Step 17 used to crash the kernel; see "Bugs this demo found" below.

## What is on the wire

vm3 recorded every packet on the segment during the demo
([full output](wire-check.txt)):

```
secure-channel packets on UDP 7100: 1261, between 6 address pairs
  HS1  initiator: X25519 + ML-KEM-768 public keys                    x5
  HS2  responder: X25519 + ML-KEM ciphertext + encrypted identity    x3
  HS3  initiator: encrypted identity + signature                     x5
  DATA ChaCha20-Poly1305 frame                                       x1248

encrypted payload: 48701 bytes, entropy 7.996 bits/byte (8.0 = indistinguishable from random)
plaintext 'index.html'             not present anywhere in the capture
plaintext 'Shusha Workspace'       not present anywhere in the capture
plaintext 'Reviewed by the admin'  not present anywhere in the capture
plaintext 'shusha-team'            not present anywhere in the capture
plaintext 'alice'                  not present anywhere in the capture
plaintext 'bob'                    not present anywhere in the capture
```

The file's name, its text, the admin's edit, the workspace name and the
member names never cross the network in the clear. DATA frames are mostly
presence pings, every 2 s per member.

## Bugs this demo found

Driving three full desktops through sign-in, editing and sign-out exposed
three kernel bugs inherited from ICDA. All are fixed (docs/PLAN.md, "Gaps
found while building"):

- **The machine froze under load.** The timer interrupt was acknowledged
  after the scheduler ran. A busy program (Argon2id at sign-in, the editor
  redrawing) stopped the clock.
- **Closing a window freed its memory too early.** The window's shared
  buffer was freed while the app could still draw into it. After a
  sign-out, those frames held the new session's process records, and the
  kernel faulted when guest signed in (step 17).
- **Exited programs were never freed.** Their pages, page tables, stacks
  and records were all kept. `scripts/reclaim-test.py` measures this with
  `/dev/meminfo`:
  - the old kernel lost 994 frames (about 4 MiB) over 30 runs of
    `ws status`;
  - Shusha loses 4.

## Automated checks

| Test | Machines | Checks | Latest run |
|---|---|---|---|
| `scripts/ws-lan-test.py`: ranks, invites, signed log, rank access, request/grant, editing and versions, admin offline, personal files stay personal | 5 | 44 | [all passed](ws-lan-test.txt) |
| `scripts/auth-test.py`: kernel gates, setup, no plaintext, lockout, recovery key, text login, re-auth, private homes, sign-out | 1 | 25 + 11 in seccheck | [all passed](auth-test.txt) |
| `scripts/reclaim-test.py`: memory comes back when programs exit | 1 | 3 | [all passed](reclaim-test.txt) |
| `make editor-test`: Emmet, language recognition, tokenizers, outline | host | 54 | passes |
| `make test`: crypto test vectors | host | 245 | passes |

## Reproduce

```bash
make SERIAL_MIRROR=1 shusha.iso lan.iso
python3 scripts/ws-gui-demo.py              # 17 screenshots -> build/wsshots
python3 scripts/wire-check.py build/lan/vm3.pcap "index.html" "Shusha Workspace"
python3 scripts/ws-lan-test.py              # 5 machines, 44 checks
python3 scripts/auth-test.py                # accounts, 25 checks
python3 scripts/reclaim-test.py             # memory reclaim
make test editor-test
```

## Limits of this proof

- **LAN only.**
- **One workspace per machine**, bound to one local user.
- **State lives in RAM** on the live image: accounts and files last until
  reboot.
- **The machine key is a random file**, not yet bound to the hardware ID.
- **Not built yet:**
  - removing members and rekeying;
  - stewards;
  - live co-editing with presence;
  - encrypted home folders.
- **No outside review yet** of the cryptography and code.
