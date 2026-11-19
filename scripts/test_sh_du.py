#!/usr/bin/env python3
"""Compiles scripts/test_sh_du.c against src/sh_du.c on the host and runs it.

Usage: python3 -I scripts/test_sh_du.py
"""

import os
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def main():
    with tempfile.TemporaryDirectory() as d:
        exe = os.path.join(d, "test_sh_du")
        subprocess.run(
            [
                "cc",
                "-std=gnu11",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-g",
                "-fsanitize=address,undefined",
                "-I",
                os.path.join(ROOT, "include"),
                "-o",
                exe,
                os.path.join(ROOT, "scripts", "test_sh_du.c"),
                os.path.join(ROOT, "src", "sh_du.c"),
            ],
            check=True,
        )
        try:
            return subprocess.run([exe], timeout=60).returncode
        except subprocess.TimeoutExpired:
            print("sh_du: timed out")
            return 124


if __name__ == "__main__":
    sys.exit(main())
