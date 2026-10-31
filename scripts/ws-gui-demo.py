#!/usr/bin/env python3
"""GUI demo on three QEMU machines: accounts, the Workspace and Code.

    make SERIAL_MIRROR=1 shusha.iso && python3 scripts/ws-gui-demo.py [outdir]

Boots three desktops on one virtual LAN and drives them with the mouse
and keyboard: first-boot account setup and sign-in, an administrator
adding a local user, creating and joining the workspace, writing HTML
with Emmet in Code and saving it as an encrypted workspace file, an edit
by a higher rank arriving live, a locked file, request and grant, a
read-only editor, CSS/Python editing, the command palette, the lock
screen, and another local user who sees nothing of the workspace.  A
screenshot is saved after each step (docs/poc/README.md); vm3's traffic
goes to build/lan/vm3.pcap for scripts/wire-check.py.  Progress and the
invite codes are read from the serial log (test builds mirror the
console there).
"""
import os
import re
import socket
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from lan import Lan, KEYS, ROOT  # noqa: E402
from PIL import Image  # noqa: E402

OUT = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "build", "wsshots")


class Screen:
    """Mouse, keyboard and screendump for one VM through its QEMU monitor."""

    def __init__(self, lan, k):
        self.s = socket.socket(socket.AF_UNIX)
        self.s.connect(lan.path(k, "sock"))
        time.sleep(0.3)
        self.drain()
        self.pos = [0, 0]

    def drain(self):
        self.s.setblocking(False)
        try:
            while self.s.recv(65536):
                pass
        except OSError:
            pass
        self.s.setblocking(True)

    def cmd(self, c, wait=0.06):
        self.s.send((c + "\n").encode())
        time.sleep(wait)
        self.drain()

    def typ(self, text):
        for ch in text:
            self.cmd("sendkey " + KEYS.get(ch, ch if not ch.isupper() else "shift-" + ch.lower()), 0.045)

    def shot(self, name):
        p = os.path.join(OUT, name)
        self.cmd("screendump %s.ppm" % p, 1.2)
        Image.open(p + ".ppm").save(p + ".png")
        os.remove(p + ".ppm")
        print(p + ".png")

    def move(self, dx, dy):
        while dx or dy:
            sx, sy = max(-40, min(40, dx)), max(-40, min(40, dy))
            self.cmd("mouse_move %d %d" % (sx, sy), 0.03)
            dx, dy = dx - sx, dy - sy

    def go(self, x, y):
        self.move(x - self.pos[0], y - self.pos[1])
        self.pos = [x, y]

    def click(self):
        self.cmd("mouse_button 1", 0.2)
        self.cmd("mouse_button 0", 0.4)

os.makedirs(OUT, exist_ok=True)

# Screen geometry on the 1280x800 desktop (see wm.c): windows open at
# content (24 + 36n, 85 + 30n) for the n-th window of a session, moved up
# to y=75 if they would reach the dock; Code is wide and sits at x=88.
DOCK = {"explorer": (550, 754), "terminal": (610, 754), "workspace": (670, 754), "code": (730, 754)}
MENU = {"accounts": (1200, 52), "lock": (1200, 78), "logout": (1200, 104)}
POWER = (1256, 15)
GREETER = (410, 166)            # login window content origin
PARK = (420, 790)


def origin(n, h):
    x, y = 24 + 36 * n, 85 + 30 * n
    if y + h > 710:
        y = 75
    return x, y


with Lan(3, iso=os.environ.get("SHUSHA_GUI_ISO", os.path.join(ROOT, "shusha.iso")), pcap=(3,)) as lan:
    time.sleep(40)
    g = {k: Screen(lan, k) for k in (1, 2, 3)}
    for k in g:
        g[k].move(-3000, -3000)

    def click(k, xy, wait=0.8):
        g[k].go(*xy)
        g[k].click()
        time.sleep(wait)

    def shot(k, name):
        g[k].go(*PARK)
        time.sleep(0.4)
        g[k].shot(name)

    def wait(k, text, mark, timeout=120):
        return lan.wait(k, text, timeout, mark)

    def key(k, name, n=1, pause=0.15):
        for _ in range(n):
            g[k].cmd("sendkey " + name, pause)

    def typ(k, text, pause=0.3):
        g[k].typ(text)
        time.sleep(pause)

    def first_boot(k, name, pw, shots=False):
        m = len(lan.log(k))
        if shots:
            shot(k, "01-first-boot-setup")
        typ(k, name + "\t" + pw + "\t" + pw + "\n")
        wait(k, "authd: created account " + name, m)
        time.sleep(1.5)
        if shots:
            shot(k, "02-recovery-key")
        typ(k, "\n", 1.5)
        m = len(lan.log(k))
        typ(k, pw + "\n")
        wait(k, "wm: session started for " + name, m)
        time.sleep(4)
        click(k, (762, 68), 1.0)  # close the Explorer window of the new session

    def in_app(o, xy):
        return (o[0] + xy[0], o[1] + xy[1])

    # ---- accounts ---------------------------------------------------------
    first_boot(1, "admin", "password1", shots=True)
    first_boot(2, "alice", "alicepass1")
    first_boot(3, "bob", "bobpass123")
    click(1, POWER, 0.8)
    shot(1, "03-user-menu")
    click(1, MENU["accounts"], 2.5)
    acc = origin(1, 560)
    click(1, in_app(acc, (660, 47)), 0.3)
    typ(1, "password1")
    click(1, in_app(acc, (364, 279)), 0.3)
    typ(1, "guest")
    click(1, in_app(acc, (541, 279)), 0.3)
    typ(1, "guestpass1")
    m = len(lan.log(1))
    click(1, in_app(acc, (733, 278)), 0.5)
    wait(1, "authd: created account guest", m)
    time.sleep(1.5)
    shot(1, "04-admin-adds-a-local-user")
    click(1, (acc[0] + 820 - 22, acc[1] - 16), 1.0)  # close Accounts

    # ---- workspace -------------------------------------------------------------
    click(1, DOCK["workspace"], 2.5)
    w1 = origin(2, 580)
    m = len(lan.log(1))
    typ(1, "shusha-team\n")
    wait(1, "workspaced: Created workspace", m)
    codes = []
    for _ in range(2):
        m = len(lan.log(1))
        click(1, in_app(w1, (690, 36)), 1.5)
        wait(1, "workspace: invite shu", m)
        codes.append(re.findall(r"invite (shu[a-z2-7]{40,})", lan.log(1)[m:])[-1])
    for k, code, name in ((2, codes[0], "alice"), (3, codes[1], "bob")):
        click(k, DOCK["workspace"], 2.5)
        wo = origin(1, 580)
        click(k, in_app(wo, (700, 239)), 0.3)
        typ(k, code)
        click(k, in_app(wo, (700, 279)), 0.3)
        typ(k, name)
        m = len(lan.log(k))
        click(k, in_app(wo, (609, 323)), 0.5)
        wait(k, "workspaced: Joined workspace", m)
        time.sleep(3)
    time.sleep(4)
    shot(2, "05-alice-joined-from-the-app")

    # ---- Code: Emmet, then save as an encrypted workspace file ----------------
    click(2, DOCK["code"], 3.0)
    key(2, "ctrl-n", 1, 0.6)
    key(2, "f1", 1, 0.6)
    typ(2, "language\n")
    typ(2, "html\n")
    typ(2, "!")
    key(2, "tab", 1, 0.8)
    typ(2, "main.page>h1{Shusha Workspace}+ul>li.item$*3", 1.0)
    shot(2, "06-code-emmet-suggestion")
    key(2, "tab", 1, 1.0)
    key(2, "f1", 1, 0.6)
    typ(2, "encrypted\n")
    key(2, "backspace", 12, 0.05)
    m = len(lan.log(2))
    typ(2, "index.html\n")
    wait(2, "workspaced: You shared index.html", m)
    time.sleep(3)
    shot(2, "07-saved-as-encrypted-workspace-file")

    # ---- the admin edits it; Alice sees it arrive -----------------------------------
    click(1, DOCK["code"], 3.0)
    key(1, "ctrl-p", 1, 0.8)
    typ(1, "index\n", 1.5)
    key(1, "ctrl-end", 1, 0.3)
    key(1, "up", 3, 0.1)
    key(1, "end", 1, 0.2)
    typ(1, "\n<footer>Reviewed by the admin", 0.6)
    m = len(lan.log(2))
    key(1, "ctrl-s", 1, 1.0)
    shot(1, "08-admin-edits-and-saves")
    wait(2, "workspaced: admin edited index.html", m)
    time.sleep(1)
    shot(2, "09-alice-sees-the-edit-arrive")

    # ---- Bob: locked, request, grant, read-only --------------------------------------
    click(3, DOCK["code"], 3.0)
    shot(3, "10-bob-sees-a-locked-file")
    click(3, DOCK["workspace"], 2.0)
    wb = origin(1, 580)
    click(3, in_app(wb, (88, 174)), 2.5)
    m = len(lan.log(2))
    click(3, in_app(wb, (768, 156)), 2.0)
    wait(2, "asks for index.html", m)
    time.sleep(1.5)
    click(2, DOCK["workspace"], 1.5)
    wa = origin(1, 580)
    click(2, in_app(wa, (88, 210)), 2.5)
    m = len(lan.log(3))
    click(2, in_app(wa, (708, 144)), 2.0)
    wait(3, "workspaced: You can now open index.html", m)
    click(3, DOCK["code"], 1.5)
    key(3, "ctrl-p", 1, 0.8)
    typ(3, "index\n", 2.0)
    shot(3, "11-bob-read-only-after-grant")

    # ---- more languages: CSS with Emmet, Python, the palette ----------------------
    click(2, DOCK["code"], 1.5)
    key(2, "ctrl-n", 1, 0.6)
    key(2, "f1", 1, 0.6)
    typ(2, "language\n")
    typ(2, "css\n")
    typ(2, ".card {\n")
    for ab in ("df", "jcsb", "p12-20", "bdrs8", "c#fc0", "bgc#1e1e1e"):
        typ(2, ab, 0.3)
        key(2, "tab", 1, 0.3)
        typ(2, "\n", 0.2)
    shot(2, "12-css-emmet")
    key(2, "ctrl-n", 1, 0.6)
    key(2, "f1", 1, 0.6)
    typ(2, "language\n")
    typ(2, "python\n")
    typ(2, "import os\n\ndef greet(name):\n", 0.3)
    typ(2, "# say hello\nreturn \"Hello, \" + name\n", 0.3)
    key(2, "backspace", 1, 0.2)
    typ(2, "\nfor user in os.listdir(\"/home\"):\n", 0.3)
    typ(2, "print(gr", 1.0)
    shot(2, "13-python-and-suggestions")
    key(2, "f1", 1, 0.6)
    typ(2, "line", 0.8)
    shot(2, "14-command-palette")
    key(2, "esc", 1, 0.5)

    # ---- lock screen, then another local user -----------------------------------------
    click(1, POWER, 0.8)
    click(1, MENU["lock"], 2.5)
    shot(1, "15-lock-screen")
    m = len(lan.log(1))
    typ(1, "password1\n")
    wait(1, "wm: unlocked", m)
    time.sleep(1)
    click(1, POWER, 0.8)
    click(1, MENU["logout"], 3.0)
    shot(1, "16-sign-in-screen-two-users")
    click(1, (GREETER[0] + 269, GREETER[1] + 144), 0.5)
    m = len(lan.log(1))
    typ(1, "guestpass1\n")
    wait(1, "wm: session started for guest", m)
    time.sleep(4)
    click(1, (762, 68), 1.0)
    click(1, DOCK["workspace"], 3.0)
    shot(1, "17-guest-sees-no-workspace")
