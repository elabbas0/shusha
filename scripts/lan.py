#!/usr/bin/env python3
"""Multi-machine LAN harness: boots N Shusha VMs on one virtual Ethernet
segment and drives their text shells.

    from lan import Lan
    with Lan(3) as lan:            # VM k gets MAC 52:54:00:12:00:0k -> 169.254.0.k
        lan.wait_all("shell ready")
        lan.type(1, "lanping 5")
        lan.wait(1, "peer(s) answered")
        print(lan.log(1))

Each VM uses the text-mode test image (make lan.iso), a serial log in
build/lan/vmK.log and a QEMU monitor socket for keystrokes.  The segment
is QEMU's multicast socket backend, so no root or bridge is needed.
"""
import os
import socket
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DIR = os.environ.get("SHUSHA_LAN_DIR", os.path.join(ROOT, "build", "lan"))
KEYS = {" ": "spc", "/": "slash", ".": "dot", "-": "minus", "\n": "ret", "\t": "tab", ",": "comma",
        ":": "shift-semicolon", "_": "shift-minus", "=": "equal", "'": "apostrophe",
        "!": "shift-1", "@": "shift-2", "#": "shift-3", "$": "shift-4", "%": "shift-5", "^": "shift-6",
        "&": "shift-7", "*": "shift-8", "(": "shift-9", ")": "shift-0", "+": "shift-equal", ";": "semicolon",
        "<": "shift-comma", ">": "shift-dot", "?": "shift-slash", "[": "bracket_left", "]": "bracket_right",
        "{": "shift-bracket_left", "}": "shift-bracket_right", '"': "shift-apostrophe", "|": "shift-backslash",
        "\\": "backslash", "`": "grave_accent", "~": "shift-grave_accent"}


class Lan:
    def __init__(self, n, iso=os.environ.get("SHUSHA_LAN_ISO", os.path.join(ROOT, "lan.iso")), mcast="230.0.0.1:1234", mem="256M", pcap=(),
                 machines=None):
        """pcap: VM numbers whose network traffic is recorded to build/lan/vmK.pcap
        (every VM receives the whole segment, so one capture sees all of it).
        machines: per-VM hardware, {k: {"disk": image, "serial": disk serial,
        "uefi": True, "cdrom": False, "uuid": SMBIOS UUID, "mac": MAC}}."""
        self.n, self.iso, self.mcast, self.mem, self.pcap = n, iso, mcast, mem, set(pcap)
        self.machines = machines or {}
        self.procs = {}

    def __enter__(self):
        os.makedirs(DIR, exist_ok=True)
        for k in range(1, self.n + 1):
            self.boot(k)
        return self

    def __exit__(self, *exc):
        self.stop()

    def path(self, k, ext):
        return os.path.join(DIR, "vm%d.%s" % (k, ext))

    def boot(self, k):
        for ext in ("log", "sock", "pcap"):
            if os.path.exists(self.path(k, ext)):
                os.remove(self.path(k, ext))
        m = self.machines.get(k, {})
        cmd = ["qemu-system-x86_64", "-cpu", "max", "-m", self.mem,
               "-display", "none", "-no-reboot",
               "-serial", "file:" + self.path(k, "log"),
               "-monitor", "unix:%s,server,nowait" % self.path(k, "sock"),
               "-netdev", self.netdev(m),
               "-device", "e1000,netdev=n0,mac=%s" % m.get("mac", "52:54:00:12:00:%02x" % k)]
        if m.get("cdrom", True):
            cmd += ["-cdrom", m.get("iso", self.iso)]
        if m.get("uefi"):
            cmd += ["-bios", "/usr/share/ovmf/OVMF.fd"]
        if m.get("uuid"):
            cmd += ["-uuid", m["uuid"]]
        if m.get("disk"):
            cmd += ["-drive", "if=none,id=d0,file=%s,format=raw" % m["disk"],
                    "-device", "ide-hd,drive=d0,bus=ide.0,serial=%s" % m.get("serial", "SHUSHA-DISK-%d" % k)]
        if k in self.pcap:
            cmd += ["-object", "filter-dump,id=f0,netdev=n0,file=" + self.path(k, "pcap")]
        self.procs[k] = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)

    def netdev(self, m):
        """The shared LAN segment, or ("nat": True) the machine alone behind
        its own NAT router (QEMU user networking: DHCP gives 10.0.2.15, the
        host's loopback is 10.0.2.2), optionally with a port forward
        ("forward": host UDP port -> the workspace port 7100)."""
        if not m.get("nat"):
            return "socket,id=n0,mcast=%s,localaddr=127.0.0.1" % self.mcast
        nd = "user,id=n0"
        if m.get("forward"):
            nd += ",hostfwd=udp:127.0.0.1:%d-:7100" % m["forward"]
        return nd

    def reboot(self, k, **hw):
        """Power VM k off and boot it again, with hardware changes (see machines)."""
        self.stop(k)
        self.machines[k] = dict(self.machines.get(k, {}), **hw)
        self.boot(k)

    def wait_exit(self, k, timeout=60):
        """Wait for VM k to power itself off or restart (QEMU runs with -no-reboot)."""
        p = self.procs.get(k)
        end = time.time() + timeout
        while p and p.poll() is None and time.time() < end:
            time.sleep(0.5)
        return p is None or p.poll() is not None

    def stop(self, k=None):
        for j in ([k] if k else list(self.procs)):
            p = self.procs.pop(j, None)
            if p and p.poll() is None:
                p.terminate()
                try:
                    p.wait(5)
                except subprocess.TimeoutExpired:
                    p.kill()

    def log(self, k):
        try:
            with open(self.path(k, "log"), "rb") as f:
                return f.read().decode("utf-8", "replace")
        except FileNotFoundError:
            return ""

    def wait(self, k, text, timeout=120, after=0):
        """Wait until `text` appears in VM k's log past offset `after`."""
        end = time.time() + timeout
        while time.time() < end:
            log = self.log(k)
            i = log.find(text, after)
            if i >= 0:
                return i + len(text)
            if self.procs.get(k) and self.procs[k].poll() is not None:
                raise RuntimeError("vm%d exited: %s" % (k, self.procs[k].stderr.read().decode()))
            time.sleep(0.5)
        raise TimeoutError("vm%d: %r not seen within %ds\n--- log tail ---\n%s" % (k, text, timeout, self.log(k)[-1500:]))

    def wait_all(self, text, timeout=180):
        for k in self.procs:
            self.wait(k, text, timeout)

    def monitor(self, k, line):
        s = socket.socket(socket.AF_UNIX)
        s.connect(self.path(k, "sock"))
        s.settimeout(2)
        try:
            s.recv(4096)
        except socket.timeout:
            pass
        s.sendall((line + "\n").encode())
        time.sleep(0.05)
        s.close()

    def type(self, k, text, enter=True):
        """Type a shell command into VM k; returns the log offset before it."""
        mark = len(self.log(k))
        s = socket.socket(socket.AF_UNIX)
        s.connect(self.path(k, "sock"))
        s.settimeout(2)
        try:
            s.recv(4096)
        except socket.timeout:
            pass
        for ch in text + ("\n" if enter else ""):
            key = KEYS.get(ch, ch if not ch.isupper() else "shift-" + ch.lower())
            s.sendall(("sendkey %s\n" % key).encode())
            time.sleep(0.04)
        s.close()
        return mark

    def run(self, k, command, until, timeout=60):
        """Type `command` and return the log text it produced up to `until`."""
        mark = self.type(k, command)
        end = self.wait(k, until, timeout, after=mark)
        return self.log(k)[mark:end]


if __name__ == "__main__":
    n = int(sys.argv[1]) if len(sys.argv) > 1 else 2
    with Lan(n) as lan:
        lan.wait_all("shell ready")
        print("all %d machines up" % n)
        marks = {k: lan.type(k, "lanping 6") for k in range(1, n + 1)}
        for k in range(1, n + 1):
            lan.wait(k, "peer(s) answered", 60, marks[k])
            out = lan.log(k)[marks[k]:]
            print("--- vm%d ---" % k)
            print("\n".join(l for l in out.splitlines() if l.startswith("lanping")))
