# Installing Shusha and keeping state

A live Shusha session (the ISO) keeps everything in memory and forgets it
at power off. Installing Shusha on a disk makes it keep accounts, homes
and the workspace, encrypted and bound to that machine. No TPM is needed
(requirement R10).

## Installing

- **On the desktop:** the user menu (top right) shows **Install Shusha**
  in a live session.
  - Pick a disk and confirm that it may be erased.
  - Enter your own password. Only administrators can install, and authd
    checks the password as for any account change.
  - The installer shows a **recovery code** once. Write it down.
- **On the text console (root):** `install` lists the disks.
  `install <disk>` installs after you type `ERASE`; `install -y <disk>`
  skips the question.

Whatever you set up in the live session is installed along with Shusha
(the first account, files, a workspace). Remove the install media and
restart: the machine boots Shusha from the disk (UEFI).

## What goes on the disk

| Partition | Contents |
|---|---|
| EFI (64 MiB, FAT32) | `\EFI\BOOT\BOOTX64.EFI` (GRUB), `\BOOT\GRUB\GRUB.CFG`, `\BOOT\KERNEL.BIN` |
| Shusha State (the rest, raw) | the encrypted state store (`kernel/fs/store.c`) |

The live kernel carries a copy of the installable kernel and a trimmed
GRUB inside itself. That copy is linked without them, so an installed
system cannot install itself again.

## The state store

Everything a machine keeps is the writable part of the file tree. The
store saves it as one image: data, owners and permission modes
(persistfs format v2). It leaves out runtime-only paths: `/dev`, request
mailboxes and the live status file.

- **Encryption.** XChaCha20-Poly1305 under a random 256-bit machine key
  (MK). The associated data binds the generation, the slot and the length.
- **Crash safety.** Two slots, used in turn. A save writes its slot, then
  that slot's commit record. At boot the newest commit whose image
  decrypts wins, so a torn or damaged save costs only that save.
- **When it saves.**
  - Changes only mark the tree dirty. ICDA instead rewrote the disk on
    every file write.
  - `syncd` saves about every 3 s if something changed. Account changes
    and power off or restart save at once.
  - A save whose content is unchanged is skipped.

```
LBA 0      keyslot: MK under hw_key; hw_key's three shares; MK under the recovery code
LBA 8, 9   commit records (generation, slot, length, nonce, tag, CRC)
LBA 64..   slot 0 | slot 1
```

## Binding to the hardware (no TPM)

`hw_key` (random) wraps MK, and `hw_key` is split 2-of-3 with Shamir's
scheme over GF(256). Share *i* is wrapped under
`SHA-256("shusha-hw-v1" || i || salt || id_i)`, where the three IDs are:

- the **SMBIOS system UUID** (GRUB reads it and passes it on the kernel
  command line as `shusha.uuid=`);
- the **network card's MAC address**;
- the **state disk's serial number** (ATA/AHCI IDENTIFY).

What happens in each case:

| Situation | Result |
|---|---|
| All three match | Opens |
| One part replaced (e.g. the network card) | Opens with 2 of 3, and the binding moves to the new part, so a second change later still works |
| The disk in another machine (only its serial matches) | **Locked**: nothing from it is loaded. The login screen asks for the recovery code. `install unlock <code>` does the same on the console |
| The recovery code | Opens MK, rebinds the disk to this machine, saves, and restarts so every service starts from the saved state |

`/dev/store` shows the state:

```
state unlocked
disk ata0 958 MiB
generation 12
saved 48213 bytes
hardware 3/3 ids match, 2 needed
ids uuid mac disk
```

Root also sees the disks, whether this kernel can install, and the
recovery code just after an install. Root can write `install <n>`,
`unlock <code>`, `sync` and `forget`. Formatting the store's disk is
refused while it is in use.

## What this protects, honestly

- **It protects against the disk being read elsewhere.** A disk taken out
  and read in another machine, or copied, shows only ciphertext. The
  test scans the raw disk for the files' contents, the workspace name and
  the home path.
- **It does not resist an attacker who knows the hardware IDs.** The IDs
  are not secrets: they are readable by software and often printed on a
  label, and a MAC address has about 24 bits of entropy once the vendor
  is known. Someone with the disk who also learns or guesses a second ID
  can open it.
- **Passwords add the next layer.**
  - Account passwords stay Argon2id hashes with a pepper inside the
    store.
  - Encrypting each home under its owner's password, so the disk alone
    never opens a home, is the next step (CRYPTO.md, L2).
- **With a TPM,** MK could additionally be sealed to it. That is optional
  and not built yet.

## Tests

`scripts/store-test.py` runs one machine. It boots the live test image
with a blank disk, installs, and checks:

- an ordinary user cannot install or list disks;
- an account, private and shared files and a workspace survive a UEFI
  boot from the disk with no install media, with their owners and modes;
- the raw disk holds none of it in the clear;
- one ID changed: the disk opens with 2 of 3, rebinds, and the next boot
  matches 3 of 3;
- the disk moved to another machine: locked and nothing loaded; a wrong
  code fails; the right code opens it, restarts, and everything is back,
  bound to the new machine;
- a damaged newest save falls back to the previous one.

`scripts/install-gui-demo.py` runs the same flow on the desktop, with
screenshots.
