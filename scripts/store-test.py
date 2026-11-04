#!/usr/bin/env python3
"""Install, reboot, and the hardware-bound state store (one machine).

    make SERIAL_MIRROR=1 lan.iso && python3 scripts/store-test.py

Boots the live test image with a blank 1 GiB disk, installs Shusha on it
(GPT: EFI partition with GRUB and the kernel, encrypted state partition),
then makes an account, private files and a workspace, and boots the disk
under UEFI with no install media:

- everything is still there, with its owners and modes;
- the raw disk holds none of it in the clear;
- one hardware ID replaced (the NIC): still opens, and rebinds;
- the disk moved to another machine (UUID and NIC differ): locked, and
  nothing is loaded; a wrong recovery code fails, the right one opens it
  and rebinds it to the new machine;
- a damaged newest save falls back to the previous one;
- ordinary users cannot install or see the recovery code.
"""
import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from lan import Lan, DIR  # noqa: E402

failures = []
UUID_A = "6b1c2d3e-4f50-4a6b-8c7d-9e0f1a2b3c4d"
UUID_B = "0d9e8f7a-6b5c-4d3e-9f2a-1b0c9d8e7f6a"
MAC_A, MAC_A2, MAC_B = "52:54:00:aa:00:01", "52:54:00:aa:00:02", "52:54:00:bb:00:01"
SERIAL = "QM-SHUSHA-01"


def check(cond, what):
    print(("  ok    " if cond else "  FAIL  ") + what)
    if not cond:
        failures.append(what)


def main():
    os.makedirs(DIR, exist_ok=True)
    disk = os.path.join(DIR, "store-disk.img")
    if os.path.exists(disk):
        os.remove(disk)
    with open(disk, "wb") as f:
        f.truncate(1 << 30)
    hw = {"disk": disk, "serial": SERIAL, "uuid": UUID_A, "mac": MAC_A}

    with Lan(1, mcast="230.0.0.13:1313", machines={1: dict(hw)}) as lan:
        def run(cmd, until="#", timeout=60):
            return lan.run(1, cmd, until, timeout)

        def answer(text, until, timeout=60):
            mark = lan.type(1, text)
            end = lan.wait(1, until, timeout, mark)
            return lan.log(1)[mark:end]

        def store():
            return run("cat /dev/store")

        def boot(**change):
            lan.reboot(1, **change)
            lan.wait(1, "shell ready", 300)
            lan.wait(1, "workspaced: ready", 60)

        lan.wait(1, "shell ready", 180)
        lan.wait(1, "authd: ready", 60)

        print("[live session]")
        out = store()
        check("state none" in out and "can-install yes" in out, "the live image saves nothing yet and can install")
        check("device 0 ata0 1024 MiB" in out, "it sees the blank disk")
        run("su 1000", "shusha:")
        out = lan.run(1, "install -y 0", "shusha:", 30)
        check("only root" in out, "an ordinary user cannot install")
        out = lan.run(1, "cat /dev/store", "shusha:", 20)
        check("device 0" not in out and "can-install" not in out, "... nor list disks through /dev/store")
        lan.run(1, "exit", "#", 20)

        print("\n[install]")
        out = run("install -y 0", "#", 300)
        code = re.search(r"([A-Z2-9]{5}-){3}[A-Z2-9]{5}", out)
        check("Shusha is installed on disk 0" in out and code is not None, "installs and shows a recovery code once")
        out = store()
        check("state unlocked" in out and "hardware 3/3" in out, "the new store is open, bound to 3 hardware IDs")
        check("recovery" not in out, "the recovery code is not kept after it was shown")

        print("\n[make some state]")
        run("users setup admin", "password: ")
        answer("password1", "#")
        out = run("users list")
        check("admin\t1000" in out, "first account made")
        run("su 1000", "shusha:")
        lan.run(1, "write /home/admin/notes.txt private-notes-4417", "shusha:", 20)
        lan.run(1, "write /home/admin/shared.txt shared-text-5521", "shusha:", 20)
        lan.run(1, "chmod 644 /home/admin/shared.txt", "shusha:", 20)
        out = lan.run(1, "ws create persist-team", "shusha:", 60)
        check("rank 0" in out, "admin creates a workspace")
        lan.run(1, "ws put /home/admin/shared.txt", "shusha:", 60)
        lan.run(1, "exit", "#", 20)
        run("install sync")
        time.sleep(4)   # syncd's round as well
        out = store()
        gen = int(re.search(r"generation (\d+)", out).group(1))
        check(gen >= 1, "saved (generation %d)" % gen)

        print("\n[boot from the disk, UEFI, no install media]")
        boot(uefi=True, cdrom=False)
        out = store()
        check("state unlocked" in out and "hardware 3/3" in out, "the installed disk boots and opens itself")
        out = run("users list")
        check("admin\t1000" in out, "the account is still there")
        run("users check admin", "password: ")
        out = answer("password1", "#")
        check("ok\t" in out, "its password still works")
        out = run("stat /home/admin/notes.txt")
        check("rw-------" in out and "owner=1000" in out, "a private file keeps its owner and mode (0600)")
        out = run("stat /home/admin/shared.txt")
        check("rw-r--r--" in out and "owner=1000" in out, "a shared file keeps its mode (0644)")
        run("su 1000", "shusha:")
        out = lan.run(1, "cat /home/admin/notes.txt", "shusha:", 20)
        check("private-notes-4417" in out, "the owner reads the private file")
        out = lan.run(1, "ws files", "shusha:", 60)
        check("persist-team" in lan.run(1, "ws status", "shusha:", 60) and "shared.txt" in out,
              "the workspace and its file are still there")
        lan.run(1, "exit", "#", 20)
        lan.stop(1)
        with open(disk, "rb") as f:
            raw = f.read()
        leaks = [w for w in (b"private-notes-4417", b"shared-text-5521", b"persist-team", b"/home/admin")
                 if w in raw]
        check(not leaks, "the raw disk holds none of it in the clear %s" % (leaks or ""))

        print("\n[one part replaced: a new network card]")
        boot(uefi=True, cdrom=False, mac=MAC_A2)
        out = store()
        check("state unlocked" in out and "hardware 2/3" in out, "2 of 3 IDs still open the disk")
        check("rebound" in out, "... and the binding moves to the new card")
        boot(uefi=True, cdrom=False, mac=MAC_A2)
        out = store()
        check("hardware 3/3" in out, "next boot: 3 of 3 again")

        print("\n[the disk moved to another machine]")
        boot(uefi=True, cdrom=False, uuid=UUID_B, mac=MAC_B)
        out = store()
        check("state locked" in out, "different UUID and NIC: the disk stays locked")
        out = run("users list")
        check("admin" not in out, "nothing from it is loaded")
        out = run("install unlock AAAAA-BBBBB-CCCCC-DDDDD")
        check("wrong recovery code" in out, "a wrong recovery code is refused")
        out = run("install unlock " + (code.group(0) if code else "x"), "restarting")
        check("unlocked" in out, "the recovery code opens it")
        check(lan.wait_exit(1, 60), "... and the machine restarts by itself")
        boot(uefi=True, cdrom=False, uuid=UUID_B, mac=MAC_B)
        out = store()
        check("state unlocked" in out and "hardware 3/3" in out, "it is now bound to the new machine")
        out = run("users list")
        check("admin\t1000" in out, "... with everything loaded")
        run("users check admin", "password: ")
        out = answer("password1", "#")
        check("ok\t" in out, "and the account's password works")

        print("\n[a damaged save]")
        run("write /home/first.txt first-save", "#", 20)
        run("install sync")
        time.sleep(4)
        run("write /home/second.txt second-save", "#", 20)
        run("install sync")
        time.sleep(4)
        out = store()
        gen = int(re.search(r"generation (\d+)", out).group(1))
        part = re.search(r"disk ata0 (\d+) MiB", out)
        lan.stop(1)
        # newest save: slot gen&1 of the state partition (kernel/fs/store.c)
        state_start = 2048 + 131072
        sectors = (((1 << 30) // 512 - 34) & ~7) - state_start
        slot = (sectors - 64) // 2
        off = (state_start + 64 + (gen & 1) * slot) * 512 + 200
        with open(disk, "r+b") as f:
            f.seek(off)
            b = f.read(1)
            f.seek(off)
            f.write(bytes([b[0] ^ 0x5a]))
        boot(uefi=True, cdrom=False, uuid=UUID_B, mac=MAC_B)
        out = store()
        check("state unlocked" in out and "previous one" in out, "the damaged newest save is skipped for the previous one")
        out = run("cat /home/first.txt") + run("cat /home/second.txt")
        check("first-save" in out and "second-save" not in out, "only the last save is lost")
        check(part is not None, "store size reported")

    print("\n%s: %d failure(s)" % ("PASS" if not failures else "FAIL", len(failures)))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
