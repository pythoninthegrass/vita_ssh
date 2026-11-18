#!/usr/bin/env python3
"""Compiles scripts/test_vpk_policy.c against src/vpk_policy.c on the host and runs it.

Usage: python3 -I scripts/test_vpk_policy.py
"""

import os
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def main():
    with tempfile.TemporaryDirectory() as d:
        exe = os.path.join(d, "test_vpk_policy")
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
                os.path.join(ROOT, "scripts", "test_vpk_policy.c"),
                os.path.join(ROOT, "src", "vpk_policy.c"),
            ],
            check=True,
        )
        return subprocess.run([exe]).returncode


if __name__ == "__main__":
    sys.exit(main())
