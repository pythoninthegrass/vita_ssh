#!/usr/bin/env python3
"""Compiles the pure formatter in src/sshd_sysinfo.c and checks what uname and /etc/os-release print.

Usage: python3 -I scripts/test_sysinfo.py
"""

import os
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

HOME_URL = "https://www.reddit.com/r/vitahacks"
SUPPORT_URL = "https://github.com/pythoninthegrass/vita_ssh"

# Required by the task. VERSION_ID has to agree with what `uname -a` prints, which
# scripts/test_sysinfo.c checks against the uname line it mirrors.
REQUIRED = ["NAME", "ID", "PRETTY_NAME", "VERSION_ID", "HOME_URL", "SUPPORT_URL"]

# Only what src/sshd_sysinfo.c touches, so the firmware call is under the check's
# control and the two surfaces can be asserted on for a firmware the host does not run.
SHIM_MODULEMGR = """/* Host shim for scripts/test_sysinfo.py: the firmware struct the formatter reads. */
#ifndef HOST_PSP2_KERNEL_MODULEMGR_H
#define HOST_PSP2_KERNEL_MODULEMGR_H
#include <stddef.h>
/* uint32_t, as the SDK's psp2common/types.h has them: with host size_t the struct
 * would pad to 0x30 and the assertion in src/sshd_sysinfo.c would be measuring a
 * different struct than the one the device passes. */
#include <stdint.h>
typedef uint32_t SceSize;
typedef uint32_t SceUInt;
typedef struct SceKernelSystemSwVersion {
    SceSize size;
    char versionString[0x1C];
    SceUInt version;
    SceUInt unk_24;
} SceKernelSystemSwVersion;
int sceKernelGetSystemSwVersion(SceKernelSystemSwVersion *version);
#endif
"""

SHIM_SYSMEM = """/* Host shim for scripts/test_sysinfo.py: the two model codes the formatter compares. */
#ifndef HOST_PSP2_KERNEL_SYSMEM_H
#define HOST_PSP2_KERNEL_SYSMEM_H
#define SCE_KERNEL_MODEL_VITA 0x10000
#define SCE_KERNEL_MODEL_VITATV 0x20000
int sceKernelGetModel(void);
#endif
"""

SHIM_BUILD_UTILS = """/* The SDK\'s own spelling, with ENABLE_VITASDK_BUILD_ASSERT on: the assert has to be
 * seen by the compiler or a wrong size passes silently, hence the attribute rather
 * than static (an unused static is a -Werror failure here). */
#ifndef HOST_VITASDK_BUILD_UTILS_H
#define HOST_VITASDK_BUILD_UTILS_H
#define VITASDK_BUILD_ASSERT(cond) ((void)sizeof(char[1 - (2 * !(cond))]))
#define VITASDK_BUILD_ASSERT_EQ(size, name) \
    __attribute__((used)) inline void __vitasdk_check_eq_##name(void) { VITASDK_BUILD_ASSERT(((size) == sizeof(name))); }
#endif
"""

SHIM_IO_STAT = """/* Host shim for scripts/test_sysinfo.py: only the stat call sh_device_present()
 * makes, and nothing else in src/sh_path.c. */
#ifndef HOST_PSP2_IO_STAT_H
#define HOST_PSP2_IO_STAT_H
#include <sys/stat.h>
/* Only the type has to exist: sh_device_present() looks at the return code, never
 * at a field. The names avoid st_* because host <sys/stat.h> macros those tokens
 * away. */
typedef struct SceIoStat {
    unsigned int mode;
    unsigned long long size;
    unsigned int attr;
    unsigned int reserved[10];
} SceIoStat;
int sceIoGetstat(const char *file, SceIoStat *stat);
#endif
"""

# sceIoGetstat succeeds for every dev: name, so a removable device is not silently
# dropped from the check's view of the device table.
IO_STUB = """
#include "psp2/io/stat.h"
#include <string.h>
int sceIoGetstat(const char *file, SceIoStat *st) {
    if (file == NULL || strchr(file, ':') == NULL) return -1;
    memset(st, 0, sizeof(*st));
    st->mode = 0x10;
    return 0;
}
"""

STUBS = """/* Host stubs for scripts/test_sysinfo.py. */
#include "psp2/kernel/modulemgr.h"
#include "psp2/kernel/sysmem.h"
#include <stdlib.h>
#include <string.h>
int sceKernelGetSystemSwVersion(SceKernelSystemSwVersion *v) {
    const char *raw = getenv("VITA_SW_VERSION");
    if (v == NULL || v->size != sizeof(*v)) return -1;
    if (raw != NULL && strcmp(raw, "fail") == 0) return -1;
    memset(v, 0, sizeof(*v));
    memcpy(v->versionString, "3.73000", 8); /* not the dotted form: nothing reads it */
    v->version = (unsigned)strtoul(raw != NULL ? raw : "0x03740000", NULL, 16);
    return 0;
}
int sceKernelGetModel(void) {
    const char *m = getenv("VITA_MODEL");
    return (m != NULL && strcmp(m, "vitatv") == 0) ? SCE_KERNEL_MODEL_VITATV
                                                   : SCE_KERNEL_MODEL_VITA;
}
"""


def build(d: Path, exe: Path) -> None:
    (d / "shim/psp2/kernel").mkdir(parents=True)
    (d / "shim/psp2/io").mkdir(parents=True)
    (d / "shim/vitasdk").mkdir(parents=True)
    (d / "shim/psp2/kernel/modulemgr.h").write_text(SHIM_MODULEMGR)
    (d / "shim/psp2/kernel/sysmem.h").write_text(SHIM_SYSMEM)
    (d / "shim/psp2/io/stat.h").write_text(SHIM_IO_STAT)
    (d / "shim/vitasdk/build_utils.h").write_text(SHIM_BUILD_UTILS)
    (d / "stubs.c").write_text(STUBS + IO_STUB)

    subprocess.run(
        [
            os.environ.get("CC", "cc"),
            "-std=gnu11",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-fsanitize=address,undefined",
            # The literals live here and nowhere else: the C helper asserts against
            # macros the compile line hands it.
            '-DHOME_URL="%s"' % HOME_URL,
            '-DSUPPORT_URL="%s"' % SUPPORT_URL,
            "-DREQUIRED_FIELDS=" + ",".join('"%s"' % f for f in REQUIRED),
            "-I",
            str(ROOT / "include"),
            "-I",
            str(d / "shim"),
            str(ROOT / "scripts/test_sysinfo.c"),
            str(ROOT / "src/sshd_sysinfo.c"),
            str(ROOT / "src/sh_path.c"),
            str(d / "stubs.c"),
            "-o",
            str(exe),
        ],
        check=True,
    )


def main():
    failures = 0
    with tempfile.TemporaryDirectory(prefix="sysinfo_") as tmp:
        tmp = Path(tmp)
        exe = tmp / "test_sysinfo"
        build(tmp, exe)
        # The formatter is checked for the firmware words a device can report, and the
        # read-then-format path against a stubbed kernel.
        cases = [
            ("formatter",),
            ("read", "0x03740000", "3.74"),
            ("read", "0x03600000", "3.60"),
            ("read", "0x03730000", "3.73", "vitatv"),
            ("read", "fail", "unknown"),
        ]
        for case in cases:
            env = {**os.environ, "VITA_CHECK": case[0]}
            if case[0] == "read":
                version, firmware = case[1], case[2]
                env["VITA_SW_VERSION"] = version
                env["VITA_READ_FIRMWARE"] = firmware
                env["VITA_READ_BUILD_ID"] = "unknown" if firmware == "unknown" else version.upper().replace("0X", "0x")
                if len(case) > 3:
                    env["VITA_MODEL"] = env["VITA_READ_MODEL"] = case[3]
            if subprocess.run([str(exe)], cwd=ROOT, env=env).returncode != 0:
                failures += 1
                print(f"case {case!r} failed")
    if failures:
        print(f"{failures} case(s) failed")
        return 1
    print("ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
