"""Mouse, keyboard and screendump for one QEMU VM through its monitor
(shared by the GUI demos)."""
import os
import socket
import time

from lan import KEYS

OUT = "."


class Screen:
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
        from PIL import Image
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
