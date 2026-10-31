#!/usr/bin/env python3
"""Accounts and privilege tests on one Shusha machine (text console).

    make SERIAL_MIRROR=1 lan.iso && python3 scripts/auth-test.py

The test image starts a root shell.  From it: the kernel's syscall gates
(seccheck), first-account setup, Argon2id hashes on disk (no plaintext),
lockout after wrong passwords, then the real text login: sign in as the
administrator, add an account (re-entering the admin password), check
private homes and the account database, sign out, sign in as the new
user, recover a password with the recovery key.
"""
import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from lan import Lan  # noqa: E402

failures = []


def check(cond, what):
    print(("  ok    " if cond else "  FAIL  ") + what)
    if not cond:
        failures.append(what)


def main():
    with Lan(1, mcast="230.0.0.7:1277") as lan:
        lan.wait(1, "shell ready", 180)
        lan.wait(1, "authd: ready", 60)

        def run(cmd, until, timeout=60):
            return lan.run(1, cmd, until, timeout)

        def answer(text, until, timeout=60):
            mark = lan.type(1, text)
            end = lan.wait(1, until, timeout, mark)
            return lan.log(1)[mark:end]

        print("[kernel gates]")
        out = run("seccheck", "seccheck: ")
        for line in out.splitlines():
            if line.strip().startswith(("ok", "FAIL")):
                print("    " + line.strip())
        check("FAIL" not in out, "an ordinary user is refused every privileged call")

        print("\n[first account]")
        run("users setup admin", "password: ")
        out = answer("password1", "#")
        key = re.search(r"([A-Z2-9]{5}-){3}[A-Z2-9]{5}", out)
        check(key is not None, "setup creates the administrator and shows a recovery key")
        out = run("users list", "#")
        check("admin\t1000\tadmin\t/home/admin" in out, "admin is uid 1000, an administrator, home /home/admin")
        out = run("stat /home/admin", "#")
        check("rwx------" in out and "owner=1000" in out, "the home is private (0700) and theirs")
        out = run("cat /var/auth/users", "#")
        check("password1" not in out, "the database holds no plaintext password")

        print("\n[wrong passwords lock the account]")
        for i in range(3):
            run("users check admin", "password: ")
            out = answer("wrongpass%d" % i, "#")
            check("wrong password" in out, "wrong password %d refused" % (i + 1))
        run("users check admin", "password: ")
        out = answer("password1", "#")
        check("too many attempts" in out, "after 3 failures even the right password waits")
        time.sleep(7)
        run("users check admin", "password: ")
        out = answer("password1", "#")
        check(out.count("ok\t") == 1, "after the lockout the right password works")

        print("\n[recovery key]")
        run("users recover admin", "recovery key: ")
        answer(key.group(0) if key else "x", "new password: ")
        out = answer("password2", "#")
        new_key = re.search(r"([A-Z2-9]{5}-){3}[A-Z2-9]{5}", out)
        check(new_key is not None and key and new_key.group(0) != key.group(0),
              "the recovery key sets a new password and is replaced by a new key")
        run("users recover admin", "recovery key: ")
        answer(key.group(0) if key else "x", "new password: ")
        out = answer("password3", "#")
        check("wrong" in out, "the used recovery key no longer works")

        print("\n[text login as the administrator]")
        run("/sbin/login.app", "shusha login: ")
        answer("admin", "password: ")
        out = answer("password2", "shusha:")
        check("Welcome, admin" in out, "admin signs in on the console")
        out = run("id", "shusha:")
        check("uid=1000" in out, "the shell runs as uid 1000, not root")
        run("users add bob", "your password: ")
        answer("nottheadminpw", "password: ")
        out = answer("bobpassword", "shusha:")
        check("wrong password" in out, "adding an account needs the admin's own password")
        run("users add bob", "your password: ")
        answer("password2", "password: ")
        out = answer("bobpassword", "shusha:")
        check(re.search(r"ok\t1001\t", out) is not None, "with it, bob is added as uid 1001")
        out = run("ls /home/bob", "shusha:")
        check("no such directory" in out, "admin cannot list bob's private home")
        out = run("cat /var/auth/users", "shusha:")
        check("cannot read" in out, "admin (not root) cannot read the account database")
        out = run("seccheck", "shusha:")
        check("start it as root" in out, "programs started by admin are not root")
        out = answer("exit", "shusha login: ")
        check("Signed out" in out, "exit signs out")

        print("\n[the second user]")
        answer("bob", "password: ")
        out = answer("bobpassword", "shusha:")
        check("Welcome, bob" in out, "bob signs in")
        out = run("id", "shusha:")
        check("uid=1001" in out, "bob's shell is uid 1001")
        out = run("ls /home/admin", "shusha:")
        check("no such directory" in out, "bob cannot list admin's home")
        run("users add eve", "your password: ")
        answer("bobpassword", "password: ")
        out = answer("evepassword", "shusha:")
        check("not an administrator" in out, "bob is not an administrator")
        out = answer("exit", "shusha login: ")
        answer("admin", "password: ")
        out = answer("password1", "shusha login: ")
        check("Login incorrect" in out, "the old password no longer signs in")

    print("\n%s: %d failure(s)" % ("PASS" if not failures else "FAIL", len(failures)))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
