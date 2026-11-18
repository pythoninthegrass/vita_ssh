#!/usr/bin/env python3
"""Writes a small VPK for exercising `vpk list` and `vpk install` without a real app.

Usage: python3 -I scripts/make_vpk_fixture.py OUT.vpk --title TITLEID [--eboot-size N] [--magic TEXT]

The package holds a param.sfo with the given TITLE_ID and an empty CONTENT_ID, and an eboot.bin of N
bytes (default 8192) that starts with TEXT (default SCE, followed by a NUL). It is not a runnable app.
"""

import argparse
import struct
import sys
import zipfile


def make_sfo(title):
    """A minimal PSF image holding TITLE_ID and an empty CONTENT_ID, as vita-mksfoex writes them."""
    entries = [("CONTENT_ID", b"\0", 4), ("TITLE_ID", title.encode() + b"\0", 16)]
    keys = b"".join(k.encode() + b"\0" for k, _, _ in entries)
    key_off = 20 + 16 * len(entries)
    data_off = key_off + len(keys)
    data_off += (-data_off) % 4
    index, data, koff, doff = b"", b"", 0, 0
    for key, value, maxlen in entries:
        index += struct.pack("<HHIII", koff, 0x0204, len(value), maxlen, doff)
        data += value.ljust(maxlen, b"\0")
        koff += len(key) + 1
        doff += maxlen
    header = b"\0PSF" + struct.pack("<IIII", 0x0101, key_off, data_off, len(entries))
    return header + index + keys.ljust(data_off - key_off, b"\0") + data


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("out")
    ap.add_argument("--title", required=True)
    ap.add_argument("--eboot-size", type=int, default=8192)
    ap.add_argument("--magic", default=None, help="first bytes of eboot.bin; default is SCE and a NUL")
    args = ap.parse_args()

    magic = b"SCE\0" if args.magic is None else args.magic.encode()
    eboot = (magic + b"\0" * args.eboot_size)[: args.eboot_size]
    with zipfile.ZipFile(args.out, "w", zipfile.ZIP_STORED) as z:
        z.writestr("sce_sys/param.sfo", make_sfo(args.title))
        z.writestr("eboot.bin", eboot)
    return 0


if __name__ == "__main__":
    sys.exit(main())
