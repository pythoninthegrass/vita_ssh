#!/usr/bin/env python3
"""Compiles scripts/test_sftp_truncate.c against src/sftp_open_policy.h on the host and runs it.

Usage: python3 -I scripts/test_sftp_truncate.py
"""
import os
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def main():
    with tempfile.TemporaryDirectory() as d:
        exe = os.path.join(d, "test_sftp_truncate")
        subprocess.run(
            ["cc", "-Wall", "-Wextra", "-Werror", "-I", os.path.join(ROOT, "src"), "-o", exe,
             os.path.join(ROOT, "scripts", "test_sftp_truncate.c")],
            check=True,
        )
        return subprocess.run([exe, d]).returncode


if __name__ == "__main__":
    sys.exit(main())
