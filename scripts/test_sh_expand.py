#!/usr/bin/env python3
"""Compiles scripts/test_sh_expand.c against src/sh_expand.c on the host and runs it.

Usage: python3 -I scripts/test_sh_expand.py
"""

import os
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def main():
    with tempfile.TemporaryDirectory() as d:
        exe = os.path.join(d, "test_sh_expand")
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
                os.path.join(ROOT, "scripts", "test_sh_expand.c"),
                os.path.join(ROOT, "src", "sh_expand.c"),
            ],
            check=True,
        )
        return subprocess.run([exe]).returncode


if __name__ == "__main__":
    sys.exit(main())
