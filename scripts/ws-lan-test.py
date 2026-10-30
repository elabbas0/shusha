#!/usr/bin/env python3
"""End-to-end Workspace test on a virtual LAN of five Shusha machines.

    make SERIAL_MIRROR=1 lan.iso && python3 scripts/ws-lan-test.py

vm1 creates the workspace (rank 0, admin) and issues invites; vm2..vm5
join in order and must get ranks 1..4.  Then: a forged and a reused
invite are refused, every machine sees the same verified member list, a
rank-2 file is readable by ranks 0-2 and locked for ranks 3-4, an access
request is granted, and with the admin powered off the members still
read replicas and share new files among themselves.
"""
import re
import sys
import time

sys.path.insert(0, __import__("os").path.dirname(__file__))
from lan import Lan  # noqa: E402

N = 5
PROMPT = "\nroot@shusha"
failures = []


def check(cond, what):
    print(("  ok    " if cond else "  FAIL  ") + what)
    if not cond:
        failures.append(what)


def ws(lan, k, args, timeout=90):
    out = lan.run(k, "ws " + args, PROMPT, timeout)
    body = out.split("\n", 1)[1] if "\n" in out else ""
    body = body[: body.rfind(PROMPT)] if PROMPT in body else body
    print("  vm%d$ ws %s\n%s" % (k, args, "".join("        " + l + "\n" for l in body.strip("\n").splitlines())), end="")
    return body


def main():
    with Lan(N) as lan:
        lan.wait_all("shell ready")
        for k in range(1, N + 1):
            lan.wait(k, "workspaced: ready", 60)
        print("%d machines up\n" % N)

        print("[create + invite + join]")
        check("rank 0 (admin)" in ws(lan, 1, "create shusha-team"), "vm1 creates the workspace as admin")
        codes = {}
        for k in range(2, N + 1):
            out = ws(lan, 1, "invite")
            m = re.search(r"shu[a-z2-7]{40,}", out)
            check(m is not None, "invite code for vm%d" % k)
            codes[k] = m.group(0)

        forged = codes[2][:-6] + ("a" if codes[2][-6] != "a" else "b") + codes[2][-5:]
        check("refused" in ws(lan, 2, "join %s mallory" % forged), "forged invite is refused")

        for k in range(2, N + 1):
            if k == 3:
                check("refused" in ws(lan, 3, "join %s reuse" % codes[2]), "vm2's used invite is refused for vm3")
            out = ws(lan, k, "join %s user%d" % (codes[k], k))
            check("as rank %d" % (k - 1) in out, "vm%d joins as rank %d" % (k, k - 1))
        time.sleep(6)  # members pull the log after each join notification

        print("\n[members agree on the signed log]")
        for k in range(1, N + 1):
            out = ws(lan, k, "members")
            ranks = re.findall(r"rank (\d)", out)
            check(ranks == ["0", "1", "2", "3", "4"], "vm%d sees ranks 0-4" % k)

        print("\n[rank-based file access]")
        lan.run(3, "write /home/plan.txt rank-two-plan", PROMPT, 20)
        check("replicated to 4 of 4" in ws(lan, 3, "put /home/plan.txt"), "vm3 (rank 2) shares plan.txt")
        time.sleep(4)
        for k, can in ((1, True), (2, True), (3, True), (4, False), (5, False)):
            out = ws(lan, k, "cat plan.txt" if can else "files")
            if can:
                check("rank-two-plan" in out, "rank %d reads a rank-2 file" % (k - 1))
            else:
                check("locked file" in out and "rank-two-plan" not in out, "rank %d cannot open a rank-2 file" % (k - 1))

        print("\n[request + grant]")
        check("access requested" in ws(lan, 4, "request rank2"), "rank 3 asks the owner for access")
        time.sleep(1)
        check("rank 3 asks for plan.txt" in ws(lan, 3, "requests"), "owner sees the request")
        check("granted" in ws(lan, 3, "grant plan.txt 3"), "owner grants rank 3")
        time.sleep(4)
        check("rank-two-plan" in ws(lan, 4, "cat plan.txt"), "rank 3 reads it after the grant")
        check("locked file" in ws(lan, 5, "files"), "rank 4 is still locked out")

        print("\n[admin offline]")
        lan.stop(1)
        print("  vm1 (admin) powered off")
        check("rank-two-plan" in ws(lan, 2, "cat plan.txt"), "rank 1 still reads the replica")
        lan.run(5, "write /home/notes.txt rank-four-notes", PROMPT, 20)
        out = ws(lan, 5, "put /home/notes.txt", 120)
        check("replicated to 3 of 4" in out, "rank 4 shares a file to the 3 online members")
        time.sleep(4)
        for k in (2, 3, 4):
            check("rank-four-notes" in ws(lan, k, "cat notes.txt"), "rank %d reads it with the admin offline" % (k - 1))

    print("\n%s: %d failure(s)" % ("PASS" if not failures else "FAIL", len(failures)))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
