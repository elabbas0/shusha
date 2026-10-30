#!/usr/bin/env python3
"""GUI demo of the Workspace on three QEMU machines, with screenshots.

    make SERIAL_MIRROR=1 shusha.iso && python3 scripts/ws-gui-demo.py [outdir]

Boots three desktops on one virtual LAN, opens a Terminal on each by
clicking the dock, and drives the scenario in docs/poc/README.md through
the Terminal's built-in `ws`, saving a screenshot after each step.  vm3's
network traffic is recorded to build/lan/vm3.pcap for scripts/wire-check.py.
Invite codes are read from the serial log (test builds mirror the
Terminal's `ws` output there).
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
with Lan(3, iso=os.path.join(ROOT, "shusha.iso"), pcap=(3,)) as lan:
    time.sleep(35)
    g = {k: Screen(lan, k) for k in (1, 2, 3)}
    for k in g:  # close Explorer, open Terminal from the dock
        g[k].move(-3000, -3000); g[k].go(762, 68); g[k].click(); time.sleep(0.8)
        g[k].go(670, 754); g[k].click(); time.sleep(2)
        g[k].go(420, 640)
    def run(k, c, until=None, wait=1.2):
        mark = len(lan.log(k))
        g[k].typ(c + "\n")
        if until: lan.wait(k, until, 90, mark)
        time.sleep(wait)
        return lan.log(k)[mark:]
    def shot(k, name): g[k].shot(name)

    run(1, "ws create shusha-team", "admin)")
    run(1, "ws invite", "shu")
    c2 = re.findall(r"shu[a-z2-7]{40,}", lan.log(1))[-1]
    run(1, "ws invite", "shu")
    c3 = re.findall(r"shu[a-z2-7]{40,}", lan.log(1))[-1]
    assert c2 != c3
    shot(1, "01-vm1-admin-creates-and-invites")

    run(2, "ws join %s alice" % c2, "rank")
    run(3, "ws join %s bob" % c3, "rank")
    time.sleep(4)
    run(2, "ws members", "you")
    shot(2, "02-vm2-alice-joins-rank1")
    run(3, "ws members", "you")
    shot(3, "03-vm3-bob-joins-rank2")

    run(2, "clear")
    run(2, "write /home/budget.txt Q3 budget 42000 AZN - rank 1 and above")
    run(2, "ws put /home/budget.txt", "replicated")
    time.sleep(3)
    shot(2, "04-vm2-alice-shares-budget")

    run(1, "clear")
    run(1, "ws members", "you")
    run(1, "ws files", "rank")
    run(1, "ws cat budget.txt", "AZN")
    shot(1, "05-vm1-admin-reads-lower-rank-file")

    run(3, "clear")
    run(3, "ws files", "rank")
    run(3, "ws cat budget.txt", "no ")
    run(3, "ws request rank1", "request")
    shot(3, "06-vm3-bob-locked-out-and-requests")

    run(2, "ws requests", "asks")
    run(2, "ws grant budget.txt 2", "replicated")
    time.sleep(3)
    shot(2, "07-vm2-alice-grants-bob")

    run(3, "ws cat budget.txt", "AZN")
    shot(3, "08-vm3-bob-reads-after-grant")

    lan.stop(1)
    time.sleep(1)
    run(3, "clear")
    run(3, "write /home/notes.txt bob draft - shared while the admin is offline")
    run(3, "ws put /home/notes.txt", "replicated", wait=2)
    shot(3, "09-vm3-admin-offline-bob-shares")
    run(2, "clear")
    run(2, "ws cat notes.txt", "offline")
    run(2, "ws status", "rank")
    shot(2, "10-vm2-admin-offline-alice-reads")
