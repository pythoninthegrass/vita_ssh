---
id: TASK-021
title: >-
  Report real system info: derive uname from the firmware API and add
  /etc/os-release
status: To Do
assignee: []
created_date: '2026-11-17 04:18'
updated_date: '2026-11-17 04:20'
labels:
  - shell
  - builtins
dependencies: []
references:
  - 'https://www.freedesktop.org/software/systemd/man/latest/os-release.html'
  - 'src/sshd_builtins.c:1724'
documentation:
  - $VITASDK/arm-vita-eabi/include/psp2/kernel/modulemgr.h
  - $VITASDK/arm-vita-eabi/include/psp2common/kernel/modulemgr.h
  - $VITASDK/arm-vita-eabi/include/psp2/kernel/sysmem.h
  - scripts/test_vpk_zip.py
  - AGENTS.md
modified_files:
  - src/sshd_builtins.c
  - src/sshd_sysinfo.c
  - include/sshd_sysinfo.h
  - include/sh_path.h
  - src/sh_path.c
  - CMakeLists.txt
  - README.md
  - scripts/test_sysinfo.py
priority: medium
ordinal: 48000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
The shell's `uname` lies. `cmd_uname` prints a hardcoded line (`sh_printf(s, "Vita vita 3.73 VitaSDK armv7a\n")`) whatever firmware the device runs, so anyone scripting against the server, or reading a bug report that quotes it, gets a firmware number that was never measured. It has already misled a real debugging session: a device whose own plugin log reported 0x03740000 was reported as 3.73 by `uname`.

Wanted outcome: the server reports real, checkable system information, and it reports it the way people already expect to ask for it. Two surfaces, one source of truth:
1. `uname` derives its release from the system firmware instead of a literal.
2. `cat /etc/os-release` prints a linux-style os-release block describing the device.

Both must read the same values from the same place, so they can never disagree - that is the point of the task rather than an aesthetic preference. `/etc` does not exist on this platform (the path layer maps a first component to a Vita device and fails ENOENT otherwise), so the shell gains a small set of synthetic read-only pseudo-files alongside the existing synthetic `/`.

Machine-local identifiers must not be baked into any field or into this task: endpoints, addresses and the maintainer's name live in CLAUDE.local.md only. Do not add an IP address or a hostname to os-release.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 `uname -a` prints the firmware version the device reports, not a literal: a device on 3.74 prints 3.74 and a device on 3.60 prints 3.60.
- [ ] #2 Bare `uname` still prints `Vita` as it does today, so existing scripts are not broken.
- [ ] #3 `uname -a` and `cat /etc/os-release` take their version from one shared function, so the two cannot report different values.
- [ ] #4 `cat /etc/os-release` prints os-release formatted KEY=value lines covering at least NAME, ID, PRETTY_NAME, VERSION_ID, HOME_URL and SUPPORT_URL, with VERSION_ID matching `uname -a`.
- [ ] #5 The os-release output is machine-parseable: one assignment per line, valid identifiers, no spaces around `=`, so `grep '^VERSION_ID='` and shell sourcing both work.
- [ ] #6 `cat /etc/os-release` includes exactly SUPPORT_URL="https://github.com/pythoninthegrass/vita_ssh" and HOME_URL="https://www.reddit.com/r/vitahacks", written as literals in the code rather than derived from the device.
- [ ] #7 `ls /etc` lists os-release, `stat /etc/os-release` reports a regular file with the size actually printed, and `ls /` still lists the device table exactly as before.
- [ ] #8 `/etc` is shell-only: SFTP for /etc/os-release fails as any unknown path does, and any other name under /etc (for example /etc/shadow) fails ENOENT with the message in the command's own voice.
- [ ] #9 `cat /etc/os-release > /ux0/data/x` puts the same bytes in the file that `cat` printed to the terminal, and `>>` appends them.
- [ ] #10 If the firmware query fails, both surfaces print an honest unknown value rather than a made-up version, and the commands still exit 0.
- [ ] #11 Apart from the two fixed project URLs above, no machine-local identifier (endpoint, address, hostname or the maintainer's name) appears in any emitted field.
- [ ] #12 A host-runnable check under scripts/ exercises the formatting for a normal firmware value, a second different firmware value and the failure path, built with the host cc like the other scripts/test_*.py checks.
- [ ] #13 `help uname`, the README command table and any README mention of uname describe the new behaviour and document /etc/os-release.
<!-- AC:END -->

## Implementation Plan

<!-- SECTION:PLAN:BEGIN -->
## Verified ground truth (checked in this session, do not re-derive)

**Firmware API.** `sceKernelGetSystemSwVersion(SceKernelSystemSwVersion *)` is declared in `$VITASDK/arm-vita-eabi/include/psp2/kernel/modulemgr.h`. The struct lives in `psp2common/kernel/modulemgr.h:108`:

```c
typedef struct SceKernelSystemSwVersion {
    SceSize size;              // set to sizeof(struct) before the call
    char    versionString[0x1C];
    SceUInt version;           // 0x0MMNPP00 encoding
    SceUInt unk_24;
} SceKernelSystemSwVersion;    // VITASDK_BUILD_ASSERT_EQ(0x28, ...)
```

Derive the release from `version` (e.g. 0x03740000 -> 3.74) rather than trusting `versionString`, whose formatting is not the dotted form, but print both on a device run before deciding. The SDK header notes a spoofed firmware returns the spoofed value - document that rather than fighting it.

**Stub library - this one will bite you.** Verified with `arm-vita-eabi-nm --defined-only`: `sceKernelGetSystemSwVersion` exists only in `libSceKernelModulemgr_stub.a` (and the ForDriver variant), **not** in `SceLibKernel_stub.a` which the server already links. Add `SceKernelModulemgr_stub` to the server's `target_link_libraries` (CMakeLists.txt, server block).

**Model.** `sceKernelGetModel()` (`psp2/kernel/sysmem.h`) is already linkable - `SceSysmem_stub` is in the server list. It only distinguishes `SCE_KERNEL_MODEL_VITA` (0x10000) from `SCE_KERNEL_MODEL_VITATV` (0x20000). PCH-1000 vs PCH-2000 would need `sceAppUtilSystemParamGetString` with a param id the SDK headers do **not** define; treat a PCH-xxxx field as optional with a local constant, not a blocker.

**A known device is on 3.74.** A plugin log on it reported 0x03740000 while `uname` said 3.73. So on that device a printed 3.73 after this change means the hardcode survived. Endpoints are in CLAUDE.local.md; never copy them into tracked files, commit messages or task notes.

## Field set

Fixed literals for the two URLs - they describe the project, not the device, so do not derive or decorate them:

```
NAME="PlayStation Vita"
VERSION="<fw>"                       # dotted, from the firmware word
ID=psvita
ID_LIKE=vitasdk
VERSION_ID="<fw>"                    # must equal what uname -a prints
BUILD_ID="0x0MMNPP00"                # the raw word
PRETTY_NAME="PlayStation Vita <fw>"
VITA_MODEL=vita|vitatv
VITA_SSHD_BUILD="<__DATE__ __TIME__>"
HOME_URL="https://www.reddit.com/r/vitahacks"
SUPPORT_URL="https://github.com/pythoninthegrass/vita_ssh"
```

Quote values the way the freedesktop os-release examples do (the two URLs above are the user's chosen values, confirmed public: `gh api repos/pythoninthegrass/vita_ssh` reports private=false). Emit no other URL. The maintainer handle appears only inside SUPPORT_URL, which is the one place attribution genuinely helps; nothing else on that list may carry a machine-local identifier.

## Shape of the change

- Put the read and the formatting in one new unit (`src/sshd_sysinfo.c` + `include/sshd_sysinfo.h`) with a pure formatter that takes a firmware word and emits strings. Both `cmd_uname` and the pseudo-file call it - that is how the shared-source acceptance criterion is satisfied structurally, not by luck.
- New `.c` files are listed by hand in the **server** `add_executable` list in CMakeLists.txt; nothing globs. Do not add it to the client list.
- For `/etc`: reuse the synthetic-target idea already used for `/`. `include/sh_path.h:45-47` defines `SH_TARGET_ROOT` / `SH_TARGET_DEVICE` / `SH_TARGET_PATH`; add a pseudo-file kind plus a small name table rather than strcmp-ing inside each builtin, so `cat`, `ls`, `stat` and redirection cannot disagree. `resolve_arg` is the single funnel and `cmd_cat` already branches on `SH_TARGET_ROOT` at `src/sshd_builtins.c:685`.
- Pseudo-files are read-only. `cat /etc/os-release > /ux0/data/x` must read the pseudo-file and write a real file, while `echo hi > /etc/os-release` must fail rather than appear to succeed. Check where `>` and `>>` resolve their target.
- Keep `/etc` out of SFTP: the SFTP handlers translate through `sh_path_sce()`, which must keep failing for a non-device first component so libssh's stock handlers never see it. Verify with a real scp/sftp attempt, not by reading code.
- A build stamp field follows the existing `sftp: server code built` line in sshd.log; remember a rebuild needs `task --force build` before the stamp changes.
- Output only through `sh_printf` (never printf/stdout), and `sh_printf` has no `%zu` - cast to unsigned and use `%u`.

## Checks

- Fast compile check: `cmake --build build --target ssh_server_vita`. Packaging: `task build`; after touching the link list use `task --force build`.
- Host check: mirror the shape of `scripts/test_vpk_zip.py` in `scripts/test_sysinfo.py` - compile the pure formatter with the host cc against fake firmware words (at least two distinct versions and the failure path) and assert the emitted lines, including the two URL fields.
- Device: compare `uname -a` and `cat /etc/os-release` against Settings -> System -> PS Vita System Software Version, and transcribe the real output into the task notes.
<!-- SECTION:PLAN:END -->

## Definition of Done
<!-- DOD:BEGIN -->
- [ ] #1 `task build` produces both ssh_client_vita.vpk and ssh_server_vita.vpk.
- [ ] #2 Behaviour checked on a device and the observed output transcribed into the task notes; device behaviour is the only real verification here.
<!-- DOD:END -->
