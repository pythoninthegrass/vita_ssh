#!/usr/bin/env python3
"""Compiles scripts/test_sh_sed.c against src/sh_sed.c and src/sh_grep.c on the host and runs it.

Usage: timeout 120 python3 -I scripts/test_sh_sed.py

After the C checks pass, the same binary is run as a sed over stdin for a table of worked
examples and every one is compared with real GNU sed (gsed), so a wrong expectation is
caught rather than copied. gsed is optional: absent, the pinning is skipped quietly.
"""

import itertools
import os
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# (arguments, stdin); every case is one the shell's subset implements.
CASES = [
    (["-e", "s/old/new/"], "old old\nkeep\nold\n"),
    (["-e", "s/old/new/g"], "old old\nkeep\nold\n"),
    (["-e", "s|/ux0/a|/ux0/b|"], "/ux0/a/x\n"),
    (["-e", "s/x*/-/g"], "abc\n"),
    (["-e", "s/a*/x/g"], "baaac\n"),
    (["-e", "s/a*/x/g"], "\n"),
    (["-e", "s/x*/-/"], "abc\n"),
    (["-e", "s/^/> /"], "a\n\n"),
    (["-e", "s/$/ </"], "a\n"),
    (["-e", "s/^$/EMPTY/"], "a\n\nb\n"),
    (["-e", "s/b*c/[&]/"], "abbcd\n"),
    (["-e", "s/a/\\&\\\\/"], "a\n"),
    (["-e", "s/[0-9]\\{2,\\}/N/g"], "a1b22c333\n"),
    (["-e", "s/a\\+/X/g"], "baaacaa\n"),
    (["-e", "s/\\(ab\\)*/X/g"], "ababcab\n"),
    (["-e", "s/a\\|ab/X/g"], "abab\n"),
    (["-n", "-e", "p"], "a\nb\n"),
    (["-e", "p"], "a\nb\n"),
    (["-e", "d"], "a\nb\n"),
    (["-e", "q"], "a\nb\nc\n"),
    (["-n", "-e", "q"], "a\nb\nc\n"),
    (["-n", "-e", "s/a/X/;p;q"], "a\nb\n"),
    (["-e", "s/a/b/", "-e", "s/b/c/"], "a\n"),
    (["-e", "s/a/b/;s/b/c/;p"], "a\n"),
    (["-e", "d;p"], "a\n"),
]


def pin(exe):
    gsed = shutil.which("gsed")
    if gsed is None:
        print("skip pinning: gsed not installed")
        return 0
    bad = 0
    # Every {a,b} line up to four bytes against a few scripts that stress empty matches.
    lines = ["".join(p) for n in range(0, 5) for p in itertools.product("ab", repeat=n)]
    extra = [(["-e", s], "".join(line + "\n" for line in lines)) for s in (
        "s/a*/X/g", "s/b*/X/", "s/a*b/X/g", "s/\\(ab\\)*/X/g", "s/a\\|b/X/g", "s/^a*/X/g",
        "s/a*$/X/g", "s/.*/[&]/", "s/^$/E/", "s/b\\?/X/g",
    )]
    for args, data in CASES + extra:
        want = subprocess.run([gsed, *args], input=data.encode(), stdout=subprocess.PIPE, timeout=20)
        got = subprocess.run([exe, *args], input=data.encode(), stdout=subprocess.PIPE, timeout=20)
        if want.stdout != got.stdout:
            bad += 1
            print(f"FAIL pin {args}: gsed {want.stdout!r} ours {got.stdout!r}")
    return bad


def main():
    with tempfile.TemporaryDirectory() as d:
        exe = os.path.join(d, "test_sh_sed")
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
                os.path.join(ROOT, "scripts", "test_sh_sed.c"),
                os.path.join(ROOT, "src", "sh_sed.c"),
                os.path.join(ROOT, "src", "sh_grep.c"),
            ],
            check=True,
        )
        rc = subprocess.run([exe], timeout=100).returncode
        if rc != 0:
            return rc
        return 1 if pin(exe) else 0


if __name__ == "__main__":
    sys.exit(main())
