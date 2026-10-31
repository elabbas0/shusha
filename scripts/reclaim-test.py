#!/usr/bin/env python3
"""Memory comes back when programs exit (one Shusha machine, text console).

    make SERIAL_MIRROR=1 lan.iso && python3 scripts/reclaim-test.py

ICDA never freed an exited program's pages, page tables, kernel stacks or
records, and left its dead threads in the run queue: on that kernel 30
runs of `ws status` cost 994 frames (about 4 MiB).  This starts programs
over and over and reads /dev/meminfo (free and total 4 KiB frames) around
each batch: the free count must come back to where it was.
"""
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from lan import Lan  # noqa: E402

PROMPT = "#"
ROUNDS = 30
SLACK = 16  # frames the kernel may keep for its own tables (64 KiB)
failures = []


def check(cond, what):
    print(("  ok    " if cond else "  FAIL  ") + what)
    if not cond:
        failures.append(what)


def main():
    with Lan(1, mcast="230.0.0.9:1299") as lan:
        lan.wait(1, "shell ready", 180)
        lan.wait(1, "workspaced: ready", 60)

        def free():
            out = lan.run(1, "cat /dev/meminfo", PROMPT, 30)
            m = re.search(r"free (\d+)\s+total (\d+)", out)
            return (int(m.group(1)), int(m.group(2))) if m else (None, None)

        def batch(cmd, what, until=PROMPT):
            for _ in range(3):  # warm-up: caches, heap, first-use tables
                lan.run(1, cmd, until, 60)
            before, total = free()
            for _ in range(ROUNDS):
                lan.run(1, cmd, until, 60)
            after, _ = free()
            if before is None or after is None:
                check(False, "%s: /dev/meminfo is readable" % what)
                return
            lost = before - after
            print("    %s: free %d -> %d of %d frames (%+d over %d runs)" % (what, before, after, total, -lost, ROUNDS))
            check(lost <= SLACK, "%s %d times keeps its memory (lost %d frames, at most %d)" % (what, ROUNDS, lost, SLACK))

        print("[programs that exit]")
        f, total = free()
        check(f is not None and 0 < f < total, "/dev/meminfo reports free and total frames")
        batch("ws status", "running `ws status`")
        batch("seccheck", "running seccheck (drops to a user, is refused)", "seccheck: all")

    print("\n%s: %d failure(s)" % ("PASS" if not failures else "FAIL", len(failures)))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
