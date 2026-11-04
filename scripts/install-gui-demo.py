#!/usr/bin/env python3
"""Desktop demo: install Shusha from the live session, boot the disk, move it.

    make SERIAL_MIRROR=1 shusha.iso && python3 scripts/install-gui-demo.py [outdir]

One QEMU machine with a blank 1 GiB disk:
  1. live session: first-boot account, the live-session note, the user
     menu's "Install Shusha", the installer, the recovery code;
  2. power off, boot the disk (UEFI, no install media): the account is
     there; the Storage window shows the encrypted, hardware-bound store;
  3. the disk in "another machine" (different UUID and network card): the
     login screen asks for the disk's recovery code; it unlocks, restarts,
     and the account is back.
Screenshots go to outdir (default build/installshots).
"""
import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from lan import Lan, ROOT, DIR  # noqa: E402

import gui_screen as gui  # noqa: E402

OUT = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "build", "installshots")
os.makedirs(OUT, exist_ok=True)
gui.OUT = OUT

POWER = (1256, 15)
MENU_Y = {"accounts": 52, "storage": 78, "lock": 104, "logout": 130, "restart": 156, "poweroff": 182}
PARK = (420, 790)
UUID_A = "6b1c2d3e-4f50-4a6b-8c7d-9e0f1a2b3c4d"
UUID_B = "0d9e8f7a-6b5c-4d3e-9f2a-1b0c9d8e7f6a"

os.makedirs(DIR, exist_ok=True)
disk = os.path.join(DIR, "install-demo.img")
if os.path.exists(disk):
    os.remove(disk)
with open(disk, "wb") as f:
    f.truncate(1 << 30)
hw = {"disk": disk, "serial": "QM-SHUSHA-77", "uuid": UUID_A, "mac": "52:54:00:aa:00:07", "uefi": True}

with Lan(1, iso=os.environ.get("SHUSHA_GUI_ISO", os.path.join(ROOT, "shusha.iso")), mcast="230.0.0.17:1317",
         machines={1: dict(hw)}) as lan:
    g = None

    def attach(delay=40):
        global g
        time.sleep(delay)
        g = gui.Screen(lan, 1)
        g.move(-3000, -3000)

    def click(xy, wait=0.8):
        g.go(*xy)
        g.click()
        time.sleep(wait)

    def shot(name):
        g.go(*PARK)
        time.sleep(0.4)
        g.shot(name)

    def typ(text, pause=0.3):
        g.typ(text)
        time.sleep(pause)

    def wait(text, mark, timeout=180):
        return lan.wait(1, text, timeout, mark)

    def menu(item, pause=2.5):
        click(POWER, 0.8)
        try:
            click((1200, MENU_Y[item]), pause)
        except (BrokenPipeError, ConnectionResetError):
            pass   # power off / restart: the machine is gone mid-click

    attach()
    m = len(lan.log(1))
    shot("01-live-first-boot")
    typ("admin\tpassword1\tpassword1\n")
    wait("authd: created account admin", m)
    time.sleep(1.5)
    typ("\n", 1.5)
    m = len(lan.log(1))
    typ("password1\n")
    wait("wm: session started for admin", m)
    time.sleep(4)
    click((762, 68), 1.0)                 # close the session's Explorer

    click(POWER, 0.8)
    shot("02-user-menu-install")
    click((1200, MENU_Y["storage"]), 3.0)
    shot("03-installer")
    # installer content origin: the session's 2nd window (wm.c cascade)
    ox, oy = 60, 115
    click((ox + 40, oy + 294), 0.4)        # "Erase everything on this disk"
    click((ox + 100, oy + 357), 0.3)       # password field
    typ("password1")
    m = len(lan.log(1))
    click((ox + 435, oy + 357), 1.0)       # Erase and install
    wait("authd: installed", m, 300)
    time.sleep(2)
    shot("04-installed-recovery-code")
    # The code exists only on screen (and in the installer's memory): read
    # it back from guest RAM to type it in later, as a person would.
    mem = os.path.join(DIR, "install-demo.mem")
    g.cmd("pmemsave 0 268435456 \"%s\"" % mem, 8.0)
    with open(mem, "rb") as f:
        found = re.findall(rb"[A-Z2-9]{5}-[A-Z2-9]{5}-[A-Z2-9]{5}-[A-Z2-9]{5}", f.read())
    os.remove(mem)
    found = [c for c in found if c != b"XXXXX-XXXXX-XXXXX-XXXXX"]   # the login screen's placeholder text
    code = found[0].decode() if found else "XXXXX-XXXXX-XXXXX-XXXXX"
    print("recovery code on screen:", code)
    menu("poweroff", 1.0)
    lan.wait_exit(1, 60)

    print("boot the installed disk")
    lan.reboot(1, cdrom=False)
    attach(45)
    shot("05-installed-sign-in")
    m = len(lan.log(1))
    typ("password1\n")
    wait("wm: session started for admin", m)
    time.sleep(4)
    click((762, 68), 1.0)
    menu("storage", 3.0)
    shot("06-storage")
    menu("poweroff", 1.0)
    lan.wait_exit(1, 60)

    print("the disk in another machine")
    lan.reboot(1, uuid=UUID_B, mac="52:54:00:bb:00:07")
    attach(45)
    shot("07-disk-from-another-machine")
    st = lan.log(1)
    print("store:", re.findall(r"state store [^\n]*", st))
    m = len(lan.log(1))
    typ("AAAAA-BBBBB-CCCCC-DDDDD\n", 2.0)
    shot("08-wrong-recovery-code")
    for _ in range(30):
        g.cmd("sendkey backspace", 0.03)
    typ(code + "\n", 1.5)
    shot("09-unlocked-restarting")
    lan.wait_exit(1, 60)
    print("restarted after the unlock")
    lan.reboot(1)
    attach(45)
    shot("10-sign-in-on-the-new-machine")
    m = len(lan.log(1))
    typ("password1\n")
    wait("wm: session started for admin", m)
    time.sleep(4)
    click((762, 68), 1.0)
    menu("storage", 3.0)
    shot("11-storage-rebound")
    print("done")
