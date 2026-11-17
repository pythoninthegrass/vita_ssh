#!/usr/bin/env python3
"""Compares src/vpk_head.c against an independent Python transcription of VitaShell's makeHeadBin.

Usage: python3 -I scripts/test_vpk_head.py
"""
import hashlib
import os
import struct
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TEMPLATE = os.path.join(ROOT, "third_party/vitashell/head.bin")


def fpkg_hmac(data):
    sha = hashlib.sha1(data).digest()
    buf = bytearray(64)
    buf[0:8] = sha[4:12]
    buf[8:16] = sha[4:12]
    buf[16:20] = sha[12:16]
    buf[20] = sha[16]
    buf[21] = sha[1]
    buf[22] = sha[2]
    buf[23] = sha[3]
    buf[24:32] = buf[16:24]
    return hashlib.sha1(bytes(buf)).digest()[:16]


def expected(template, content_id):
    h = bytearray(template)
    cid = content_id.encode()[:48]
    h[0x30:0x30 + 48] = cid.ljust(48, b"\0")
    be = lambda off: struct.unpack(">I", h[off:off + 4])[0]
    n = be(0xD0)
    h[n:n + 16] = fpkg_hmac(bytes(h[0:n]))
    off, n, out = be(0x08), be(0x10), be(0xD4)
    h[out:out + 16] = fpkg_hmac(bytes(h[off:off + n - 64]))
    n = be(0xE8)
    h[n:n + 16] = fpkg_hmac(bytes(h[0:n]))
    return bytes(h)


def main():
    template = open(TEMPLATE, "rb").read()
    with tempfile.TemporaryDirectory() as d:
        exe = os.path.join(d, "test_vpk_head")
        subprocess.run(["cc", "-std=gnu11", "-Wall", "-Wextra", "-g", "-fsanitize=address,undefined",
                        "-I", os.path.join(ROOT, "include"),
                        os.path.join(ROOT, "scripts/test_vpk_head.c"), os.path.join(ROOT, "src/vpk_head.c"),
                        "-o", exe], check=True)
        failures = 0
        for cid in ("EP9000-ROM000070_00-0000000000000000", "X" * 60, ""):
            out = os.path.join(d, "out.bin")
            rc = subprocess.run([exe, TEMPLATE, cid, out]).returncode
            if rc != 0 or open(out, "rb").read() != expected(template, cid):
                print("FAIL content id %r" % cid[:20])
                failures += 1
        bad = os.path.join(d, "short.bin")
        open(bad, "wb").write(template[:0x100])
        if subprocess.run([exe, bad, "x", os.path.join(d, "o")]).returncode != 1:
            print("FAIL short template accepted")
            failures += 1
        if failures == 0:
            print("ok")
        sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
