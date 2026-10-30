#!/usr/bin/env python3
"""Inspect a capture of the workspace LAN (Lan(..., pcap=(k,)) writes
build/lan/vmK.pcap) and show what an eavesdropper on the wire gets.

    python3 scripts/wire-check.py build/lan/vm3.pcap "secret text" ...

Prints the secure-channel packets by type, one hex sample of each, the
byte entropy of the encrypted payloads, and whether any of the given
plaintext strings appear anywhere in the capture.  Exit status 1 if one
does.
"""
import collections
import math
import struct
import sys

PORT = 7100
TYPES = {1: "HS1  initiator: X25519 + ML-KEM-768 public keys",
         2: "HS2  responder: X25519 + ML-KEM ciphertext + encrypted identity",
         3: "HS3  initiator: encrypted identity + signature",
         4: "DATA ChaCha20-Poly1305 frame"}


def packets(path):
    with open(path, "rb") as f:
        data = f.read()
    magic = struct.unpack("<I", data[:4])[0]
    endian = "<" if magic in (0xA1B2C3D4, 0xA1B23C4D) else ">"
    off = 24
    while off + 16 <= len(data):
        _, _, incl, _ = struct.unpack(endian + "IIII", data[off:off + 16])
        yield data[off + 16:off + 16 + incl]
        off += 16 + incl


def udp(frame):
    if len(frame) < 42 or frame[12:14] != b"\x08\x00" or frame[23] != 17:
        return None
    ihl = (frame[14] & 15) * 4
    u = 14 + ihl
    sport, dport, ulen = struct.unpack(">HHH", frame[u:u + 6])
    return frame[26:30], frame[30:34], sport, dport, frame[u + 8:u + ulen]


def entropy(b):
    if not b:
        return 0.0
    c = collections.Counter(b)
    return -sum(n / len(b) * math.log2(n / len(b)) for n in c.values())


def main():
    path, needles = sys.argv[1], [s.encode() for s in sys.argv[2:]]
    raw = b""
    count = collections.Counter()
    sample = {}
    payload = bytearray()
    flows = set()
    for fr in packets(path):
        raw += fr
        u = udp(fr)
        if not u or PORT not in (u[2], u[3]) or not u[4]:
            continue
        src, dst, _, _, p = u
        t = p[0]
        count[t] += 1
        flows.add((".".join(map(str, src)), ".".join(map(str, dst))))
        sample.setdefault(t, p)
        if t == 4:
            payload += p[17:]  # after type, session id and counter
    print("capture: %s" % path)
    print("secure-channel packets on UDP %d: %d, between %d address pairs" % (PORT, sum(count.values()), len(flows)))
    for t in sorted(count):
        print("  %-66s x%-4d (first one %d bytes)" % (TYPES.get(t, "type %d" % t), count[t], len(sample[t])))
    for t in sorted(sample):
        p = sample[t]
        print("\nsample %s:" % TYPES.get(t, t).split()[0])
        for i in range(0, min(len(p), 96), 32):
            print("  " + " ".join("%02x" % b for b in p[i:i + 32]))
        if len(p) > 96:
            print("  ... %d more bytes" % (len(p) - 96))
    print("\nencrypted payload: %d bytes, entropy %.3f bits/byte (8.0 = indistinguishable from random)"
          % (len(payload), entropy(bytes(payload))))
    leaked = [n for n in needles if n in raw]
    for n in needles:
        print("plaintext %-50r %s" % (n.decode(), "FOUND ON THE WIRE" if n in raw else "not present anywhere in the capture"))
    return 1 if leaked else 0


if __name__ == "__main__":
    sys.exit(main())
