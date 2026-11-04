#!/usr/bin/env python3
"""The workspace over the internet: four machines, each behind its own NAT.

    make SERIAL_MIRROR=1 lan.iso && python3 scripts/internet-test.py

Every VM sits alone behind its own NAT router (QEMU user networking): it
gets 10.0.2.15 by DHCP, so all four have the same private address, and
none can be reached from outside.  The only way in is a port forward on
vm1's router (like a home router's), which plays the public internet's
address here.

- vm1 creates the workspace and sets its public endpoint; invites carry it.
- vm2 and vm4 join over the "internet".  vm1 introduces them to each other
  and they reach each other directly through their NATs (no relay).
- vm3 is behind a strict NAT (`ws network relay`): it reaches the others
  only through vm1, which forwards frames it cannot read.
- Files replicate over all of these paths; with vm1 powered off, vm2 and
  vm4 keep sharing directly.
"""
import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from lan import Lan  # noqa: E402

PROMPT = "shusha:"
failures = []
FWD = 17100 + os.getpid() % 2000


def check(cond, what):
    print(("  ok    " if cond else "  FAIL  ") + what)
    if not cond:
        failures.append(what)


def main():
    machines = {k: {"nat": True} for k in range(1, 5)}
    machines[1]["forward"] = FWD
    with Lan(4, machines=machines) as lan:
        def run(k, cmd, timeout=90):
            out = lan.run(k, cmd, PROMPT, timeout)
            body = out.split("\n", 1)[1] if "\n" in out else ""
            print("  vm%d$ %s\n%s" % (k, cmd, "".join("        " + l + "\n" for l in body.strip("\n").splitlines()[:12])), end="")
            return body

        lan.wait_all("shell ready")
        for k in range(1, 5):
            lan.wait(k, "workspaced: ready", 90)
        print("4 machines up, each behind its own NAT\n")

        print("[behind NAT]")
        ips = [re.search(r"ip (\S+)", run(k, "cat /dev/netinfo")) for k in range(1, 5)]
        check(all(m and m.group(1) == "10.0.2.15" for m in ips), "DHCP gives every machine the same private address, 10.0.2.15")

        print("\n[create, with a public endpoint]")
        check("rank 0" in run(1, "ws create net-team"), "vm1 creates the workspace")
        check("public endpoint 10.0.2.2:%d" % FWD in run(1, "ws endpoint 10.0.2.2:%d" % FWD),
              "vm1 sets its public endpoint (its router's port forward)")
        check("relay only" in run(3, "ws network relay"), "vm3 is behind a strict NAT: relay only")
        codes = {}
        for k in (2, 3, 4):
            m = re.search(r"shu[a-z2-7]{40,}", run(1, "ws invite"))
            codes[k] = m.group(0) if m else "x"

        print("\n[join over the internet]")
        for k in (2, 3, 4):
            out = run(k, "ws join %s user%d" % (codes[k], k), 120)
            check("as rank %d" % (k - 1) in out, "vm%d joins through vm1's public endpoint" % k)
        time.sleep(14)   # introductions (M_PEERS every beat) and relayed handshakes

        print("\n[paths]")
        out = run(2, "ws members")
        check(re.search(r"rank 0  \S+  direct 10\.0\.2\.2:%d" % FWD, out) is not None, "vm2 reaches vm1 directly at its public endpoint")
        check(re.search(r"rank 3  user4  direct 10\.0\.2\.2:\d+", out) is not None,
              "vm2 reaches vm4 directly through both NATs (introduced by vm1)")
        check(re.search(r"rank 2  user3  via \S+ \(relay\)", out) is not None, "vm2 reaches strict vm3 through the relay")
        out = run(3, "ws members")
        check(re.search(r"rank 1  user2  via \S+ \(relay\)", out) is not None and
              re.search(r"rank 3  user4  via \S+ \(relay\)", out) is not None, "vm3 reaches both through the relay")
        out = run(4, "ws members")
        check(re.search(r"rank 1  user2  direct", out) is not None, "vm4 reaches vm2 directly")

        print("\n[files over every path]")
        lan.run(3, "write /home/strict.txt from-behind-a-strict-nat", PROMPT, 20)
        out = run(3, "ws put /home/strict.txt", 180)
        check("replicated to 3 of 3" in out, "strict vm3 shares a file to all 3 members (one direct, two relayed)")
        time.sleep(4)
        check("from-behind-a-strict-nat" in run(2, "ws cat strict.txt"), "vm2 (rank 1) reads it, relayed")
        check("from-behind-a-strict-nat" in run(1, "ws cat strict.txt"), "vm1 (admin) reads it")
        out = run(1, "ws network")
        n = re.search(r"relayed for others: (\d+)", out)
        check(n is not None and int(n.group(1)) > 0, "vm1 relayed frames it cannot read (%s)" % (n.group(1) if n else 0))
        lan.run(4, "write /home/nat.txt nat-to-nat-direct", PROMPT, 20)
        check("replicated to 3 of 3" in run(4, "ws put /home/nat.txt", 180), "vm4 shares a file to everyone")
        time.sleep(4)
        check("nat-to-nat-direct" in run(2, "ws cat nat.txt"), "vm2 reads vm4's file")

        print("\n[the admin powered off]")
        lan.stop(1)
        time.sleep(10)
        out = run(2, "ws members")
        check("rank 0" in out and re.search(r"rank 0  \S+  offline", out) is not None, "vm2 sees vm1 offline")
        check(re.search(r"rank 3  user4  direct", out) is not None, "vm2 and vm4 stay connected directly")
        lan.run(4, "write /home/after.txt shared-with-the-admin-offline", PROMPT, 20)
        out = run(4, "ws put /home/after.txt", 180)
        check("replicated to 1 of" in out or "replicated to 2 of" in out, "vm4 shares a file with the admin offline")
        time.sleep(4)
        check("shared-with-the-admin-offline" in run(2, "ws cat after.txt"), "vm2 reads it: NAT to NAT, no admin, no relay")

    print("\n%s: %d failure(s)" % ("PASS" if not failures else "FAIL", len(failures)))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
