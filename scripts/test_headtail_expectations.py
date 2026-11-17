#!/usr/bin/env python3
"""Pins the head/tail builtin expectations against GNU coreutils, on the host.

The builtin's option loop is static inside src/sshd_builtins.c and cannot be linked on
the host, so the grammar is checked separately by scripts/test_sh_headtail.py (which
drives src/sh_headtail.c) and what this file checks is the expected answer itself: every
case below is run through ghead/gtail, so a wrong expectation is caught here rather than
carried into a device run. It is the same idea as the reference pin in
scripts/test_sh_headtail.py, one level up.

Run: python3 -I scripts/test_headtail_expectations.py
"""

import os
import subprocess
import sys
import tempfile


def gnu(tool, args, data):
    """Runs ghead/gtail over data and returns (returncode, stdout)."""
    with tempfile.TemporaryDirectory() as d:
        path = os.path.join(d, "f")
        with open(path, "wb") as fh:
            fh.write(data)
        proc = subprocess.run([tool] + args + [path], capture_output=True)
        return proc.returncode, proc.stdout


L12 = ("\n".join(f"L{i}" for i in range(1, 13)) + "\n").encode()

CASES = [
    # tool, argv, file bytes, expected stdout, expected returncode (None: unchecked)
    #
    # The defaults, which only show up when the file is longer than the default.
    ("head", [], L12, ("\n".join(f"L{i}" for i in range(1, 11)) + "\n").encode(), 0),
    ("tail", [], L12, ("\n".join(f"L{i}" for i in range(3, 13)) + "\n").encode(), 0),
    # -n with a space and attached, and a short enough count to be the whole file.
    ("head", ["-n", "2"], b"one\ntwo\nthree\n", b"one\ntwo\n", 0),
    ("head", ["-n2"], b"one\ntwo\nthree\n", b"one\ntwo\n", 0),
    ("tail", ["-n", "2"], b"one\ntwo\nthree\n", b"two\nthree\n", 0),
    ("tail", ["-n", "9"], b"one\ntwo\n", b"one\ntwo\n", 0),
    ("head", ["-n", "9"], b"one\ntwo\n", b"one\ntwo\n", 0),
    # -n 0 prints nothing either way. GNU exits 1 for it and this shell treats an empty
    # answer as success, so the status is left unchecked rather than pinned wrong.
    ("head", ["-n", "0"], b"one\ntwo\n", b"", None),
    ("tail", ["-n", "0"], b"one\ntwo\n", b"", None),
    # -c is a byte window off each end, verbatim.
    ("head", ["-c", "4"], b"one\ntwo\n", b"one\n", 0),
    ("tail", ["-c", "5"], b"one\ntwo\n", b"\ntwo\n", 0),
    ("tail", ["-c", "20"], b"abc", b"abc", 0),
    # The cases where a tool drifts: a blank line among the answers, and a last line with
    # no newline. tail -n 1 of "a\n\n" is the blank line, not "a", because the final
    # newline ends "a" rather than opening anything.
    ("head", ["-n", "2"], b"aa\n\nbaa", b"aa\n\n", 0),
    ("tail", ["-n", "1"], b"aa\n\nbaa", b"baa", 0),
    ("tail", ["-n", "2"], b"aa\n\nbaa", b"\nbaa", 0),
    ("tail", ["-n", "3"], b"aa\n\nbaa", b"aa\n\nbaa", 0),
    ("tail", ["-n", "1"], b"a\n\n", b"\n", 0),
    ("tail", ["-n", "2"], b"a\n\n", b"a\n\n", 0),
    ("tail", ["-n", "3"], b"\n\nb\nbaa", b"\nb\nbaa", 0),
    ("head", ["-n", "3"], b"\n\nb\nbaa", b"\n\nb\n", 0),
    ("tail", ["-n", "1"], b"\n", b"\n", 0),
    ("head", ["-n", "1"], b"x", b"x", 0),
    ("tail", ["-n", "1"], b"x", b"x", 0),
]


def main():
    if subprocess.run(["which", "ghead"], capture_output=True).returncode != 0:
        print("SKIP head/tail expectations: ghead not installed (brew install coreutils)")
        return 0
    failures = 0
    for tool, args, data, want, want_rc in CASES:
        rc, out = gnu(tool, args, data)
        if out != want or (want_rc is not None and rc != want_rc):
            print(f"FAIL {tool} {' '.join(args)} {data!r} -> {out!r} rc={rc}; expected {want!r} rc={want_rc}")
            failures += 1
    if failures:
        print(f"FAIL: {failures} expectation(s) disagree with coreutils")
        return 1
    print(f"PASS head/tail expectations match coreutils ({len(CASES)} cases)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
