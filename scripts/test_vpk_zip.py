#!/usr/bin/env python3
"""Builds zip fixtures, compiles scripts/test_vpk_zip.c against src/vpk_zip.c on the host, runs it.

Usage: python3 -I scripts/test_vpk_zip.py
"""
import os
import struct
import subprocess
import sys
import tempfile
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

SMALL = b"SCE\x00" + bytes(range(256)) * 3
SFO = b"\x00PSF" + b"TITLE_ID\x00ROM000070\x00" * 4
BIG = (b"vita-ssh " * 1000 + os.urandom(5000)) * 60  # several 64 KB chunks, still compressible


def write_zip(path, entries, method=zipfile.ZIP_DEFLATED):
    with zipfile.ZipFile(path, "w", method) as zf:
        for name, data, how in entries:
            info = zipfile.ZipInfo(name)
            info.compress_type = how if how is not None else method
            zf.writestr(info, data)


def patch_central(path, fn):
    """Applies fn(bytes, central_offset) -> bytes to the first central directory header."""
    raw = bytearray(open(path, "rb").read())
    cd = raw.rfind(b"PK\x01\x02")
    open(path, "wb").write(fn(raw, cd))


def build(d):
    os.makedirs(os.path.join(d, "expect/sce_sys"))
    for name, data in (("eboot.bin", SMALL), ("sce_sys/param.sfo", SFO), ("big.bin", BIG)):
        open(os.path.join(d, "expect", name), "wb").write(data)

    write_zip(os.path.join(d, "good.zip"), [
        ("eboot.bin", SMALL, zipfile.ZIP_DEFLATED),
        ("sce_sys/", b"", zipfile.ZIP_STORED),
        ("sce_sys/param.sfo", SFO, zipfile.ZIP_STORED),
        ("sce_sys/icon0.png", b"png", zipfile.ZIP_STORED),
        ("big.bin", BIG, zipfile.ZIP_DEFLATED),
    ])

    bad = os.path.join(d, "bad_crc.zip")
    write_zip(bad, [("eboot.bin", SMALL, zipfile.ZIP_STORED)])
    raw = bytearray(open(bad, "rb").read())
    raw[raw.index(SMALL) + 10] ^= 0xFF
    open(bad, "wb").write(raw)

    good = open(os.path.join(d, "good.zip"), "rb").read()
    open(os.path.join(d, "truncated.zip"), "wb").write(good[:-30])
    open(os.path.join(d, "not_a_zip.bin"), "wb").write(b"hello" * 100)

    for fixture, name in (
        ("name_dotdot", "sce_sys/../../evil"),
        ("name_abs", "/ux0/evil"),
        ("name_backslash", "a\\b"),
        ("name_device", "ux0:evil"),
        ("name_long", "d/" * 130),
    ):
        write_zip(os.path.join(d, fixture + ".zip"), [(name, b"x", zipfile.ZIP_STORED)])

    write_zip(os.path.join(d, "bzip2.zip"), [("eboot.bin", SMALL, zipfile.ZIP_BZIP2)])

    enc = os.path.join(d, "encrypted.zip")
    write_zip(enc, [("eboot.bin", SMALL, zipfile.ZIP_STORED)])

    def set_encrypted(raw, cd):
        raw[cd + 8] |= 1  # general purpose flag bit 0
        return bytes(raw)

    patch_central(enc, set_encrypted)

    oob = os.path.join(d, "oob_offset.zip")
    write_zip(oob, [("eboot.bin", SMALL, zipfile.ZIP_STORED)])

    def set_bad_offset(raw, cd):
        raw[cd + 42 : cd + 46] = struct.pack("<I", 0x7FFFFFF0)
        return bytes(raw)

    patch_central(oob, set_bad_offset)


def main():
    with tempfile.TemporaryDirectory() as d:
        build(d)
        exe = os.path.join(d, "test_vpk_zip")
        cc = ["cc", "-std=gnu11", "-Wall", "-Wextra", "-g", "-fsanitize=address,undefined",
              "-I", os.path.join(ROOT, "include"),
              os.path.join(ROOT, "scripts/test_vpk_zip.c"), os.path.join(ROOT, "src/vpk_zip.c"),
              "-lz", "-o", exe]
        subprocess.run(cc, check=True)
        sys.exit(subprocess.run([exe, d]).returncode)


if __name__ == "__main__":
    main()
