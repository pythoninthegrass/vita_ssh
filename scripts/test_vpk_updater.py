#!/usr/bin/env python3
"""Host-check the updater handoff logic with stubbed Vita app/installer APIs."""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    with tempfile.TemporaryDirectory(prefix="vpk_updater_") as tmp:
        tmp = Path(tmp)
        (tmp / "psp2/kernel").mkdir(parents=True)
        (tmp / "psp2/appmgr.h").write_text(
            "typedef int SceUID;\n"
            "int sceAppMgrGetIdByName(SceUID *, const char *);\n"
            "int sceAppMgrDestroyAppByName(const char *);\n"
            "int sceAppMgrLaunchAppByUri(int, const char *);\n"
        )
        (tmp / "psp2/kernel/processmgr.h").write_text("int sceKernelExitProcess(int);\n")
        (tmp / "stubs.c").write_text(r"""
#undef main
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vpk_update.h"
int updater_main(void);
static int env(const char *name) {
    const char *s = getenv(name);
    return s ? atoi(s) : 0;
}
int sceAppMgrGetIdByName(int *pid, const char *name) {
    if (strcmp(name, SSHD_TITLEID)) abort();
    *pid = 123;
    puts("lookup");
    return env("LOOKUP");
}
int sceAppMgrDestroyAppByName(const char *name) {
    if (strcmp(name, SSHD_TITLEID)) abort();
    puts("destroy");
    return env("DESTROY");
}
int vpk_promote(const char *path, const char **stage) {
    if (strcmp(path, VPK_UPDATE_PKG)) abort();
    *stage = "promote package";
    puts("promote");
    return env("PROMOTE");
}
int sceAppMgrLaunchAppByUri(int flags, const char *uri) {
    if (flags != 0xFFFFF || strcmp(uri, "psgm:play?titleid=" SSHD_TITLEID)) abort();
    puts("launch");
    return env("LAUNCH");
}
int sceKernelExitProcess(int status) {
    if (status) abort();
    puts("exit");
    return 0;
}
int main(void) { return updater_main(); }
""")
        stage = tmp / "stage"
        stage.mkdir()
        exe = tmp / "updater"
        subprocess.run([
            os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
            "-fsanitize=address,undefined", "-Dmain=updater_main",
            '-DSSHD_TITLEID="ROM000070"', '-DSSHD_UPDATER_TITLEID="ROM000072"',
            f'-DVPK_UPDATE_ROOT="{stage}"', "-I", str(tmp), "-I", str(ROOT / "include"),
            str(ROOT / "src/sshd_updater.c"), str(tmp / "stubs.c"), "-o", str(exe),
        ], check=True)

        def run(marker, expected, result, **env):
            ready = stage / "ready"
            if marker is not None:
                ready.write_bytes(marker)
            out = subprocess.run(
                [str(exe)], check=True, capture_output=True, text=True,
                env={**os.environ, "LOOKUP": "0", "DESTROY": "0", "PROMOTE": "0",
                     "LAUNCH": "0", **env},
            )
            assert out.stdout.splitlines() == expected, out.stdout
            assert result in (stage / "result.txt").read_text()
            assert not ready.exists(), "handoff must be consumed"

        run(None, ["launch", "exit"], "no pending update")
        run(b"ROM000071", ["launch", "exit"], "no pending update")
        run(b"ROM000070extra", ["launch", "exit"], "no pending update")
        run(b"ROM000070", ["lookup", "destroy", "promote", "launch", "exit"], "0x00000000")
        # Consumed markers must not replay when the helper bubble is tapped.
        run(None, ["launch", "exit"], "no pending update")
        run(b"ROM000070", ["lookup", "promote", "launch", "exit"],
            "0x00000000", LOOKUP="-1")
        run(b"ROM000070", ["lookup", "destroy", "launch", "exit"],
            "stop server: 0xFFFFFFFF", DESTROY="-1")
        run(b"ROM000070", ["lookup", "destroy", "promote", "launch", "exit"],
            "promote package: 0xFFFFFFFF", PROMOTE="-1")
        run(b"ROM000070", ["lookup", "destroy", "promote", "launch", "exit"],
            "launch server: 0xFFFFFFFF", LAUNCH="-1")
    print("ok")


if __name__ == "__main__":
    main()
