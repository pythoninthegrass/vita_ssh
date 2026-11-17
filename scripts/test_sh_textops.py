#!/usr/bin/env python3
"""Compiles scripts/test_sh_textops.c against src/sh_textops.c on the host and runs it.

Usage: timeout 60 python3 -I scripts/test_sh_textops.py

The C test carries its own naive reference, so the exhaustive part needs no subprocesses.
This wrapper pins that reference against real gwc, gsort and guniq on the worked examples
only -- at most one call per case, never per case -- before running the C, so a wrong
reference is caught rather than copied. The GNU tools are optional: absent, the pinning is
skipped quietly and the C still decides.
"""

import os
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# (want_lines, want_words, want_bytes) and (numeric, reverse, uniq) and
# (show_count, dups, singles) as the C test spells them, translated to GNU's flags.
WC_FLAGS = {
    (1, 1, 1): [],
    (1, 0, 0): ["-l"],
    (0, 1, 0): ["-w"],
    (0, 0, 1): ["-c"],
    (0, 1, 1): ["-w", "-c"],
}

SORT_FLAGS = {
    (0, 0, 0): [],
    (0, 0, 1): ["-u"],
    (0, 1, 0): ["-r"],
    (1, 0, 0): ["-n"],
    (1, 0, 1): ["-n", "-u"],
}

UNIQ_FLAGS = {
    (0, 0, 0): [],
    (1, 0, 0): ["-c"],
    (0, 1, 0): ["-d"],
    (0, 0, 1): ["-u"],
    (1, 1, 0): ["-c", "-d"],
    (1, 0, 1): ["-c", "-u"],
}


def run(tool, args, data):
    """One call, and the answer squeezed to the single-space form this shell prints."""
    proc = subprocess.run([tool, *args], input=data, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=20)
    return proc.returncode, proc.stdout


def wc_row(fields, name=None):
    """A wc row in this shell's format: single-space columns and, when asked for, a name."""
    head = " ".join(fields)
    row = head + " " + name if name else head
    return (row + "\n").encode()


def pin():
    """One real call per worked example, so the naive reference itself is checked.

    GNU pads wc columns, so the expected answers here are this shell's own stated format
    and the real tool's row is read back through split() before it is compared.
    """
    cases = []
    for flags, (data, fields) in {
        (1, 1, 1): (b"a b\nc\n", ["2", "3", "6"]),
        (0, 1, 1): (b"a b\nc\n", ["3", "6"]),
        (1, 0, 0): (b"a b\nc\n", ["2"]),
    }.items():
        cases.append(("gwc", WC_FLAGS[flags], data, wc_row(fields)))
    for flags, (data, want) in {
        (0, 0, 0): (b"b\na\nb\n", b"a\nb\nb\n"),
        (1, 0, 0): (b"1\n01\n", b"01\n1\n"),
        (1, 0, 0): (b"1.10\n1.9\n", b"1.10\n1.9\n"),
        (0, 0, 1): (b"b\na\nb\n", b"a\nb\n"),
        (0, 1, 0): (b"b\na\nb\n", b"b\nb\na\n"),
        (1, 0, 0): (b"10\n9\n-1\nx\n1.5\n", b"-1\nx\n1.5\n9\n10\n"),
        (1, 0, 1): (b"1\n01\n1.0\n", b"1\n"),
    }.items():
        cases.append(("gsort", SORT_FLAGS[flags], data, want))
    for flags, (data, want) in {
        (0, 0, 0): (b"a\na\nb\na\n", b"a\nb\na\n"),
        (1, 0, 0): (b"a\na\nb\na\n", b"      2 a\n      1 b\n      1 a\n"),
        (0, 1, 0): (b"a\na\nb\na\n", b"a\n"),
        (0, 0, 1): (b"a\na\nb\na\n", b"b\na\n"),
        (1, 1, 0): (b"a\na\nb\na\n", b"      2 a\n"),
        (1, 0, 1): (b"a\na\nb\na\n", b"      1 b\n      1 a\n"),
    }.items():
        cases.append(("guniq", UNIQ_FLAGS[flags], data, want))

    checked = 0
    for tool, args, data, want in cases:
        try:
            code, got = run(tool, args, data)
        except FileNotFoundError:
            print(f"skip pinning: {tool} not installed")
            return 0
        except Exception as exc:  # a hung or missing tool is a harness failure, not a pass
            print(f"FAIL pinning {tool} {args}: {exc}")
            return 1
        if tool == "gwc":
            got = wc_row(got.decode().rstrip("\n").split())
        if code != 0 or got != want:
            print(f"FAIL reference {tool} {args} on {data!r}: got {got!r} want {want!r}")
            return 1
        checked += 1
    print(f"pinned {checked} reference answers against GNU")
    return 0


def main():
    if pin() != 0:
        return 1
    with tempfile.TemporaryDirectory() as d:
        exe = os.path.join(d, "test_sh_textops")
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
                os.path.join(ROOT, "scripts", "test_sh_textops.c"),
                os.path.join(ROOT, "src", "sh_textops.c"),
            ],
            check=True,
        )
        return subprocess.run([exe]).returncode


if __name__ == "__main__":
    sys.exit(main())
