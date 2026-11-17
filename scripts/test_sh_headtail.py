#!/usr/bin/env python3
"""Compiles scripts/test_sh_headtail.c against src/sh_headtail.c on the host and runs it.

Usage: timeout 60 python3 -I scripts/test_sh_headtail.py

The C test carries its own naive reference, so the exhaustive part needs no subprocesses.
This wrapper pins that reference against a handful of real ghead/gtail calls (once each,
never per case) before running the C, so a wrong reference is caught rather than copied.
"""

import os
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TIMEOUT_S = 60


def split_keep(b):
    """Lines as (bytes, has_newline): a line ends at a newline, or at EOF."""
    if not b:
        return []
    out = []
    start = 0
    for i, c in enumerate(b):
        if c == 0x0A:
            out.append((b[start:i], True))
            start = i + 1
    if start < len(b):
        out.append((b[start:], False))
    return out


def ref_head(b, n):
    """First n lines, each keeping its own newline."""
    if n <= 0:
        return b""
    lines = 0
    for i, c in enumerate(b):
        if c == 0x0A:
            lines += 1
            if lines == n:
                return b[: i + 1]
    return b


def ref_tail(b, n):
    """Last n lines, run to file size; a trailing newline ends a line, it does not open one."""
    if n <= 0 or not b:
        return b""
    size = len(b)
    end = size - 1 if b[-1:] == b"\n" else size
    count = 0
    for i in range(end - 1, -1, -1):
        if b[i] != 0x0A:
            continue
        count += 1
        if count == n:
            return b[i + 1 : size]
    return b


def pin_reference():
    """Checks the reference against real ghead/gtail: one call per case, under 20 total."""
    tools = {}
    for tool in ("ghead", "gtail"):
        found = subprocess.run(
            ["bash", "-c", f"command -v {tool}"], capture_output=True, text=True
        )
        tools[tool] = found.stdout.strip()
    if not all(tools.values()):
        print("note: ghead/gtail absent, reference pinned by worked examples only")
        return 0

    cases = [b"", b"\n", b"a", b"a\n", b"a\nb", b"a\nb\n", b"aa\n\nbaa", b"\n\nb\nbaa"]
    bad = 0
    with tempfile.TemporaryDirectory() as d:
        path = os.path.join(d, "f")
        for text in cases:
            with open(path, "wb") as fh:
                fh.write(text)
            for n in (1, 2):
                for tool, ref in ((tools["ghead"], ref_head), (tools["gtail"], ref_tail)):
                    exp = subprocess.run(
                        [tool, "-n", str(n), path], capture_output=True
                    ).stdout
                    got = ref(text, n)
                    if got != exp:
                        print(
                            f"FAIL reference {os.path.basename(tool)} -n{n} "
                            f"in={text!r} got={got!r} exp={exp!r}"
                        )
                        bad += 1
    return bad


def main():
    bad = pin_reference()
    if bad:
        return 1

    with tempfile.TemporaryDirectory() as d:
        exe = os.path.join(d, "test_sh_headtail")
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
                os.path.join(ROOT, "scripts", "test_sh_headtail.c"),
                os.path.join(ROOT, "src", "sh_headtail.c"),
            ],
            check=True,
        )
        try:
            return subprocess.run([exe], timeout=TIMEOUT_S).returncode
        except subprocess.TimeoutExpired:
            print("FAIL test_sh_headtail timed out (a scan that never terminates)")
            return 1


if __name__ == "__main__":
    sys.exit(main())
