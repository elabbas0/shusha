# Accounts, sign-in and sessions

Shusha is multi-user, like ICDA's accounts system, but none of the code is
shared. Each part was designed again to be harder to attack.

## The pieces

| Piece | Runs as | Job |
|---|---|---|
| `authd` (`user/sbin/authd.c`) | root | Holds the accounts, checks passwords, rate-limits, creates and deletes accounts, handles recovery keys |
| `greeter` (`user/sbin/greeter.c`) | uid 1 (no home, no files) | The login and lock screen. Passes what you type to authd and never sees a hash |
| `wm` | root | Shows only the login screen until authd says who signed in, then runs that user's desktop under their uid |
| `launch` (`user/sbin/launch.c`) | root, then the user | Switches to the user's uid and home, then starts the program |
| `login` (`user/sbin/login.c`) | root | The text-console sign-in. Runs the user's shell as them and ends their programs at sign-out |
| Accounts app, `users` | the user | Change your password. Administrators can also add, reset and delete accounts |

## How it compares with ICDA

| | ICDA (accounts in the kernel) | Shusha |
|---|---|---|
| **Password hashing** | PBKDF2-HMAC-SHA256 | Argon2id, 16 MiB × 2 passes, which is memory-hard. It uses a per-account salt, a random machine pepper (Argon2's secret input), and the account name as associated data |
| **Stolen database** | Can be attacked offline | Useless without the pepper, which root alone can read. A hash cannot be moved to another account |
| **Guessing** | No limit | 3 failures lock the account for 5 s, doubling up to 15 min. During a lockout no hash is computed. Constant-time compares |
| **Where it runs** | Inside the kernel | A userspace service. The kernel only enforces uids |
| **Administrators** | Admin flag | The admin flag only grants account management in authd. Admins are not root, and every change re-asks their password |
| **Automatic sign-in** | Supported | Not offered |
| **Login screen** | Part of the desktop | A separate program running as an unprivileged uid. The window manager hides every other window and withholds all input from them until authd confirms a sign-in |
| **Recovery** | Recovery key | Recovery key (100 bits), hashed like a password. Using it replaces it |
| **Sign-out** | Ends the caller's programs | The kernel's root-only `/dev/killuid` ends every program of that user |
| **Homes** | Per-user view of /home | `/home/<name>`, mode 0700. New files are 0600 and new folders 0700 by default |

## Kernel hardening that came with it

ICDA lets any process make these calls. In Shusha, an ordinary user now
gets -1 from:
- kill, suspend or resume on someone else's program;
- disk calls: mount, format, install and partition roles;
- power, the framebuffer and the GPU;
- raw keyboard and mouse reads while the desktop is active. Desktop
  programs get input only through the window manager, so nothing can read
  what you type into the login screen;
- starting a program file they are not allowed to read.

All of this is listed in `docs/ABI-v1.md`. `seccheck` (in the image)
tries every one of these calls as an ordinary user.

## Tests

- **`scripts/auth-test.py` (one machine, 26 checks):**
  - the kernel gates;
  - first-account setup;
  - no plaintext on disk;
  - lockout;
  - recovery keys, including that a used key stops working;
  - the real text login as the administrator;
  - adding an account with re-authentication;
  - private homes;
  - the account database is unreadable to non-root users;
  - signing out, then in as the second user;
  - non-admins cannot add accounts;
  - the old password stops working.
- **`scripts/ws-gui-demo.py`:** the same flows on the desktop, with
  screenshots in `docs/poc/`.

## Limits

- **State is in RAM** on the live image. Accounts, like files, last until
  reboot until disk persistence of ownership lands.
- **The machine pepper is a random file**, not yet bound to the hardware
  ID (planned with `keyd`).
- **No encrypted home folders yet.** Homes are protected by permissions,
  not by a per-user key.
