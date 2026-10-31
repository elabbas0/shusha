#!/usr/bin/env python3
"""GUI demo of the Workspace on three QEMU machines, with screenshots.

    make SERIAL_MIRROR=1 shusha.iso && python3 scripts/ws-gui-demo.py [outdir]

Boots three desktops on one virtual LAN and drives the Workspace app with
the mouse and keyboard - create, invite, join, share, locked file, access
request and grant, admin going offline, another local user refused -
saving a screenshot after each step (docs/poc/README.md).  vm3's network
traffic is recorded to build/lan/vm3.pcap for scripts/wire-check.py.
Invite codes and progress are read from the serial log (test builds
mirror the console there).
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

# Screen positions on a 1280x800 desktop: the first window opens with its
# client area at (61, 115).
DOCK_TERMINAL, DOCK_WORKSPACE = (640, 754), (700, 754)
EXPLORER_CLOSE = (762, 68)
NAV = {"overview": (147, 217), "members": (147, 253), "files": (147, 289),
       "requests": (147, 325), "activity": (147, 361)}
CODE_FIELD, NAME_FIELD, JOIN = (750, 355), (750, 395), (668, 437)
INVITE = (750, 151)
ADD_FILE = (857, 221)
ROW1_OPEN, ROW1_SAVE, ROW1_REQUEST = (735, 271), (840, 271), (829, 271)
VIEW_BACK, VIEW_SAVE = (296, 221), (832, 221)
GRANT1 = (769, 259)
PARK = (420, 760)  # mouse resting spot, off the windows

with Lan(3, iso=os.environ.get("SHUSHA_GUI_ISO", os.path.join(ROOT, "shusha.iso")), pcap=(3,)) as lan:
    time.sleep(40)
    g = {k: Screen(lan, k) for k in (1, 2, 3)}

    def click(k, xy, wait=0.8):
        g[k].go(*xy)
        g[k].click()
        time.sleep(wait)

    def park(k):
        g[k].go(*PARK)
        time.sleep(0.4)

    def shot(k, name):
        park(k)
        g[k].shot(name)

    def wait(k, text, mark, timeout=90):
        return lan.wait(k, text, timeout, mark)

    def terminal(k, lines):
        """Open a Terminal, run some commands, close it again."""
        click(k, DOCK_TERMINAL, 2.0)
        for line in lines:
            g[k].typ(line + "\n")
            time.sleep(1.0)

    for k in g:  # close the Explorer window that opens at login
        g[k].move(-3000, -3000)
        click(k, EXPLORER_CLOSE)

    # 1. vm1 creates the workspace from the app.
    click(1, DOCK_WORKSPACE, 2.5)
    shot(1, "01-vm1-workspace-app-before")
    m = len(lan.log(1))
    g[1].typ("shusha-team\n")
    wait(1, "workspaced: Created workspace", m)
    time.sleep(3)
    shot(1, "02-vm1-admin-created")

    # 2. Two invites; codes come from the serial log.
    codes = []
    for _ in range(2):
        m = len(lan.log(1))
        click(1, INVITE, 1.5)
        wait(1, "workspace: invite shu", m)
        codes.append(re.findall(r"invite (shu[a-z2-7]{40,})", lan.log(1)[m:])[-1])
    shot(1, "03-vm1-invite-code")

    # 3. vm2 and vm3 join through the app.
    for k, code, name in ((2, codes[0], "alice"), (3, codes[1], "bob")):
        click(k, DOCK_WORKSPACE, 2.5)
        click(k, CODE_FIELD, 0.3)
        g[k].typ(code)
        click(k, NAME_FIELD, 0.3)
        g[k].typ(name)
        m = len(lan.log(k))
        click(k, JOIN, 0.5)
        wait(k, "workspaced: Joined workspace", m)
        time.sleep(4)
    shot(2, "04-vm2-alice-joined")
    time.sleep(4)
    click(1, NAV["members"], 3.0)
    shot(1, "05-vm1-members-online")

    # 4. Alice adds one of her own files.
    terminal(2, ["write /home/budget.txt Q3 budget 42000 AZN - rank 1 and above", "exit"])
    click(2, NAV["files"], 1.0)
    click(2, ADD_FILE, 1.0)
    m = len(lan.log(2))
    g[2].typ("budget.txt\n")
    wait(2, "workspaced: You shared", m)
    time.sleep(3.5)
    shot(2, "06-vm2-alice-shared-a-file")

    # 5. The admin opens it; Bob cannot.
    time.sleep(2)
    click(1, NAV["files"], 3.0)
    click(1, ROW1_OPEN, 2.0)
    shot(1, "07-vm1-admin-opens-lower-rank-file")
    click(3, NAV["files"], 3.0)
    shot(3, "08-vm3-bob-sees-a-locked-file")
    m = len(lan.log(2))
    click(3, ROW1_REQUEST, 2.0)
    wait(2, "asks for budget.txt", m)

    # 6. Alice gets the request live in her top bar and grants it.
    time.sleep(1.5)
    shot(2, "09-vm2-request-arrives-in-top-bar")
    click(2, NAV["requests"], 3.0)
    shot(2, "10-vm2-requests")
    m = len(lan.log(3))
    click(2, GRANT1, 2.0)
    wait(3, "workspaced: You can now open budget.txt", m)
    time.sleep(1.5)
    shot(3, "11-vm3-top-bar-access-granted")
    time.sleep(2)
    click(3, NAV["files"], 3.0)
    click(3, ROW1_OPEN, 2.0)
    click(3, VIEW_SAVE, 2.0)
    shot(3, "12-vm3-bob-opens-and-saves-a-copy")

    # 7. The admin goes offline; the others carry on.
    lan.stop(1)
    m = len(lan.log(2))
    wait(2, "workspaced: admin went offline", m, 60)
    time.sleep(10)
    click(2, NAV["members"], 3.0)
    shot(2, "13-vm2-admin-offline")

    # 8. Another user on Bob's machine gets nothing.
    terminal(3, ["su 1000", "ws files", "cat /home/budget.txt", "workspace"])
    time.sleep(3)
    shot(3, "14-vm3-other-user-refused")
