#!/usr/bin/env python3
"""Compiles scripts/test_sh_awk.c against src/sh_awk.c and src/sh_grep.c on the host and runs it.

Usage: timeout 120 python3 -I scripts/test_sh_awk.py

After the C checks pass, the same binary is run as an awk over stdin for a table of worked
examples and every one is compared with real awk (BWK awk and gawk, whichever are installed),
so a wrong expectation is caught rather than copied. A real awk that is absent is skipped.
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
    (["{print $1, $3}"], "a b c d\ne f g\n"),
    (["{print $0}"], "a  b\tc\n"),
    (["{print}"], "a  b\n"),
    (["{print $1, $5}"], "a b\n"),
    (["{print $NF}"], "a b c\n\nd\n"),
    (["{print NR, NF}"], "a b\nc\n\nd e f\n"),
    (["{print \"x\", 1.50, 007, 1e3}"], "a\n"),
    (["{print $1; print $2}"], "a b\n"),
    (["NR > 2"], "a\nb\nc\nd\n"),
    (["NF == 2"], "a b\nc\nd e\n"),
    (["/^[0-9]+$/"], "12\n1a\n+\n\n"),
    (["/^ab?c$/"], "ac\nabc\nabbc\n"),
    (["/^(ab|cd)$/"], "ab\ncd\nabcd\n"),
    (["/a+b/"], "a+b\naab\nb\n"),
    (["/a\\+b/"], "a+b\naab\n"),
    (["/a|b/"], "a\nb\nc\n"),
    (["/[0-9][0-9]*/"], "a1\nb\n"),
    (["$3 == \"x\""], "a b x\na b y\n"),
    (["$2 > 10"], "a 5\nb 20\nc 10\nd x\ne\n"),
    (["$2 >= 10"], "a 5\nb 20\nc 10\nd x\n"),
    (["$2 == 5"], "a 5\nb 5.0\nc 05\nd x\n"),
    (["$1 == 1.0"], "1\n1.0\n1e0\nabc\n 1 \n"),
    (["$1 > 5"], "abc\n10\n9\n\n"),
    (["$1 < \"b\""], "a\nc\n"),
    (["$1 == \"10\""], "10\n10.0\n"),
    (["$3 == \"\""], "a b\na b c\n"),
    (["$2 > -1"], "a 0\nb -2\nc\n"),
    (["5 < $1"], "4\n6\n"),
    (["-F", ":", "{print $2}"], "a:b:c\n::\n\n"),
    (["-F", ":", "{print NF}"], "a::b\na:\n:\n\n"),
    (["-F", "|", "{print $2}"], "a|b|c\n"),
    (["-F", ".", "{print $2}"], "a.b.c\n"),
    (["-F", ",", "{print $2}"], "a, b,c\n"),
    (["-F", "\\t", "{print $2}"], "a b\tc d\te\n"),
    (["-F", " ", "{print $2}"], "a   b\n"),
]


def pin(exe):
    bad = 0
    reals = [p for p in (shutil.which("awk"), shutil.which("gawk")) if p]
    if not reals:
        print("skip pinning: no awk installed")
        return 0
    # Every {a,b} line up to four bytes against programs that stress empty fields and anchors.
    lines = ["".join(p) for n in range(0, 5) for p in itertools.product("ab", repeat=n)]
    data = "".join(line + "\n" for line in lines)
    extra = [([s], data) for s in (
        "/a/", "/^a/", "/b$/", "/^$/", "/a*b/", "/(ab)+/", "/a{2}/", "/^(a|b)b/", "/a?b?$/",
        "$1 == \"a\"", "$1 < \"ab\"", "NF == 0", "{print $1, $2}", "{print NF, NR}",
        "{print $NF, $0}", "$1 > \"a\" {print NR}",
    )]
    extra += [(["-F", sep, prog], data) for sep in ("a", "b") for prog in (
        "{print NF}", "{print $1}", "{print $2, $3}", "{print $NF}", "$2 == \"\"",
    )]
    for args, text in CASES + extra:
        got = subprocess.run([exe, *args], input=text.encode(), stdout=subprocess.PIPE, timeout=20)
        for real in reals:
            want = subprocess.run([real, *args], input=text.encode(), stdout=subprocess.PIPE, timeout=20)
            if want.stdout != got.stdout:
                bad += 1
                print(f"FAIL pin {os.path.basename(real)} {args}: awk {want.stdout!r} ours {got.stdout!r}")
    return bad


def main():
    with tempfile.TemporaryDirectory() as d:
        exe = os.path.join(d, "test_sh_awk")
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
                os.path.join(ROOT, "scripts", "test_sh_awk.c"),
                os.path.join(ROOT, "src", "sh_awk.c"),
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
