---
id: TASK-001.08
title: Let vpk install accept third-party VPKs after validating them
status: Done
assignee:
  - Claude
created_date: '2026-11-18T07:14:00Z'
updated_date: '2026-11-18T08:44:00Z'
labels:
  - shell
  - deploy
dependencies: []
references:
  - src/sshd_vpk.c
  - TASK-001.07
documentation:
  - README.md
  - AGENTS.md
  - docs/development.md
modified_files:
  - src/vpk_policy.c
  - include/vpk_policy.h
  - scripts/test_vpk_policy.c
  - scripts/test_vpk_policy.py
  - scripts/make_vpk_fixture.py
  - src/sshd_vpk.c
  - src/sshd_builtins.c
  - include/sshd.h
  - include/vpk_update.h
  - CMakeLists.txt
  - scripts/smoke.sh
  - README.md
  - AGENTS.md
  - docs/development.md
parent_task_id: TASK-001
priority: low
type: enhancement
ordinal: 67000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
`vpk install` accepts only the server's own title ID (ROM000070), so any other app, such as VitaShell or a homebrew title, still needs a USB copy and an install from VitaShell on the device. The install pipeline itself is not tied to our title: TASK-001.07 recorded that the stage, head.bin and promoter route installed a package for a different, non-running title (ROM000071) from the running server, and the title check is a guard that was kept on purpose, so a mistaken VPK could not replace another app. Lifting it needs validation that makes that safe.

Constraints carried over from TASK-001.07 and its hardware runs: the system promoter refuses to replace a running title (0x80101114) and the server can only update itself through the bundled updater title; system promotion is whole-package replacement, so files omitted from a new package are removed from an already-installed title; the app controls staging cleanup but not rollback of a system-installer failure; and the project's line for third-party apps is that a running one is never closed or destroyed from an SSH command.

docs/development.md and AGENTS.md currently say `vpk install` is fully implemented for "any VPK", which is wrong while the own-title guard stands.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [x] #1 `vpk install FILE` installs a valid VPK for a title that is not the server's own and is not running, and the title is present in the installed apps afterwards, verified on a device with the sister project's Vita smoke package (title MBVK00001, staged at ux0:downloads/megabonk-vita-smoke.vpk), which stays installed for that project's own hardware checks.
- [x] #2 A package whose title is running is refused with a readable message naming the title, nothing is written for it, and no running app is closed or destroyed by the command; the refusal decision is covered by the host test and the running check is shown on a device by `vpk list` reporting the server's own title as running, because a running third-party title cannot be arranged on the device.
- [x] #3 The package is validated before the promoter is called: archive integrity, a well-formed param.sfo with a TITLE_ID, and an eboot.bin that starts with the SCE magic and is not implausibly small; each failure prints its reason and leaves the device unchanged and the stage clean.
- [x] #4 Installing over a title that is already installed is refused unless the command is given an explicit option, and the refusal says the whole package would be replaced and files omitted from the new one removed.
- [x] #5 The server's own title ROM000070 and the updater title ROM000072 are never installed through this path: the own title keeps using the updater handoff, and the updater title is refused.
- [x] #6 `vpk list` still reports whether a package is this app, another title, or a title that is already installed.
- [x] #7 The validation decisions have a host test written first, covering accepted and refused packages including the own, updater, running and already-installed cases.
- [x] #8 README, help, docs/development.md and AGENTS.md state what `vpk install` accepts; the wording that says it handles any VPK is corrected whichever way this task lands.
<!-- AC:END -->

## Definition of Done
<!-- DOD:BEGIN -->
- [x] #1 task build produces both VPKs
- [x] #2 Verified on a device over a real ssh session
<!-- DOD:END -->

## Implementation Plan

<!-- SECTION:PLAN:BEGIN -->
Researched 2026-11-18 against src/sshd_vpk.c (committed at 33abf50). Same approach as TASK-025 and TASK-018: the pure decision logic in a module with a host test written first, the glue checked on the device.

Current code: sh_vpk_install() opens the package, runs package_check() (title must equal SSHD_TITLEID, plus eboot.bin present, at least 4096 bytes), opens the bundled updater, stages the package in VPK_UPDATE_PKG and the helper in VPK_UPDATE_HELPER, writes head.bin for each, promotes the helper with vpk_promote(), writes the ready marker and sets update_pending so sh_vpk_handoff() launches the updater and exits. sh_vpk_list() prints the title ID with '(this app)' or '(not this app)'. cmd_vpk accepts exactly `vpk list FILE` or `vpk install FILE`. vpk_promote(path, &stage) is already shared and takes any staged directory with a head.bin.

1. Pure module include/vpk_policy.h and src/vpk_policy.c, no Vita APIs: vpk_title_ok(title) (nine characters from A-Z and 0-9, the first a letter: ROM000070 and MBVK00001 both qualify); vpk_decide(title, own, updater, installed, running, replace) returning one verdict: VPK_OWN (the existing updater handoff, unchanged), VPK_INSTALL (a new third-party title), VPK_REPLACE (installed, not running, replace option given), or a refusal: VPK_BAD_TITLE, VPK_REFUSE_UPDATER, VPK_REFUSE_SYSTEM (titles beginning NPXS, the system apps, which this path never overwrites), VPK_REFUSE_RUNNING, VPK_REFUSE_INSTALLED (installed, no replace option). Order: bad title, own, updater, system, running, installed. vpk_verdict_name() gives a stable word for list and tests.
2. Host test first: scripts/test_vpk_policy.c and test_vpk_policy.py under ASan/UBSan, run under timeout, like test_sh_chain: fixed cases from the ACs, plus every combination of ten title kinds (own, updater, system, a plain third-party title, and six malformed ones) with installed, running and replace flags against a table written separately. Confirm it fails before the module exists, then mutate it.
3. Glue in src/sshd_vpk.c: package_check() keeps only the content checks (eboot present, size); the title decision moves to vpk_decide(). Running is sceAppMgrGetIdByName() succeeding for the title; installed is stat() of ux0:app/TITLE (opendir answers ENOTDIR for a missing path here, so absence is asked of stat). A new install_third_party() stages in its own directory under VPK_UPDATE_ROOT, writes head.bin, calls vpk_promote() directly, and removes the stage on every path; no updater, no handoff, no marker. file_write() calls sh_session_yield() per chunk, since a third-party package can be large and the staging must not starve the poll loop (the promoter call itself cannot yield; the README says so).
4. cmd_vpk: `vpk install [-r] FILE`, -r meaning replace an installed title; the refusal for an installed title names -r and says the whole package replaces the old one and omitted files are removed. `vpk list` appends the state: this app, another title, installed, running. help and usage text updated.
5. scripts/smoke.sh exec checks for the refusals using small generated fixtures (a new scripts/make_vpk_fixture.py, snake_case, standard library only): updater title, system title, malformed title, bad eboot magic. Running a third-party title cannot happen on a device (the server is the one foreground app), so AC2's running refusal is covered by the host test and by `vpk list` reporting the server's own package as running; say so in the notes.
6. Device: install the sister project's smoke package (MBVK00001) from ux0:downloads with `vpk install`, check it appears under /ux0/app and that the server, its keys and the log are untouched; then try it again without -r (refused) and with -r.
7. README, help, docs/development.md and AGENTS.md: state what vpk install accepts, correct the 'any VPK' sentences, document -r and that the promoter call blocks the server for its duration.
8. Record arm-vita-eabi-size before (2301384) and after.

Risks: sceAppMgrGetIdByName semantics are only known from community code, so its result is verified on the device against the server's own title before anything relies on it. Replacing an installed title is whole-package replacement and cannot be rolled back by the app. The promoter call blocks the poll loop for as long as the system takes on a large package. A system title (NPXS prefix) is the one thing this must never overwrite.

Final solution matches the plan. The install uses VPK_THIRD_PKG (ux0:data/vitasshd-update/third) in include/vpk_update.h; vpk_decide() and vpk_title_ok() cover thirteen title kinds in the host test; sh_vpk_install() takes the replace flag and dispatches to install_own() (the previous body, unchanged apart from taking the opened package) or install_third_party().
<!-- SECTION:PLAN:END -->

## Implementation Notes

<!-- SECTION:NOTES:BEGIN -->
2026-11-18: test fixture chosen by the user for AC1: the sister project's Vita smoke package, title MBVK00001, already on the device at ux0:downloads/megabonk-vita-smoke.vpk (46.3 KB, copied with this repo's sftp server). `vpk list` on the current build reads it correctly: 6 files (param.sfo, an 48442-byte eboot.bin, icon0.png, livearea bg.png, startup.png and template.xml), 52122 bytes, reported as 'not this app'. This is the real case behind the task: the sister project's TASK-084.01 recorded that this server's `vpk install` rejected exactly this package because it only accepts the server's own title, leaving a VitaShell install as the only route. Installing it is what that project wants anyway, so AC1's 'removed again' was dropped: the package stays installed. Not yet installed through this server.

2026-11-18: implemented. include/vpk_policy.h and src/vpk_policy.c are the pure decision (vpk_title_ok, vpk_title_is_system, vpk_decide, vpk_verdict_name); scripts/test_vpk_policy.c and .py were written first and failed to build without the module, then passed (fixed cases from the ACs plus thirteen title kinds against all eight combinations of installed, running and replace from a literal table). Nine broken versions (running ignored, replace ignored, system allowed, updater allowed, own title gated on installed, title length off by one, digit allowed first, lowercase allowed, system prefix of three characters) each fail it. sshd_vpk.c: package_check() keeps only the content checks; title_installed() is stat() of ux0:app/TITLE, title_running() is sceAppMgrGetIdByName(); install_third_party() stages in VPK_THIRD_PKG, writes head.bin, calls vpk_promote() directly and removes the stage on every path; file_write() now calls sh_session_yield() per chunk. cmd_vpk accepts `vpk install [-r] FILE`; help text updated. Server text 2303186 against 2301384 (+1802). Host tests vpk_policy, vpk_zip, vpk_head, vpk_updater pass.

Device (the one Vita, home network). The refactored own-title path was exercised first: `vpk install` of the new build over ssh staged, handed off and relaunched (result.txt 'promote package: 0x00000000'), twice. `vpk list` of the server's own package prints 'ROM000070 (this app, running)', so sceAppMgrGetIdByName reports a running title as expected; the smoke package printed 'MBVK00001 (not this app)' before and 'MBVK00001 (not this app, installed)' after. AC1: `vpk install /ux0/downloads/megabonk-vita-smoke.vpk` unpacked 6 files (52122 bytes), installed MBVK00001 in about 5 seconds, /ux0/app/MBVK00001 holds eboot.bin and sce_sys, the server kept running, and ux0:data/vitasshd-update held only result.txt (stage removed). AC4: running the same install again was refused with a message that names the title, says the whole package is replaced and omitted files removed, and gives the -r command, exit 1; `vpk install -r` then replaced it ('Installing MBVK00001 over the installed copy', 'Installed MBVK00001'). scripts/smoke.sh exec: 114 pass 0 fail after one fix: the first run showed `vpk install -r` with no file exited 1, not 2, because argv[2] '-r' was taken as the file; cmd_vpk now treats it as a usage error. New smoke checks use packages from scripts/make_vpk_fixture.py: another title lists as 'not this app', the own title lists as 'this app, running', the updater title is refused (exit 1, 'bundled updater'), a system title NPXS99999 is refused and nothing appears in /ux0/app, a malformed title ID is refused, a bad eboot magic and an undersized eboot are refused, no 'third' directory is left behind, and -r with no file is a usage error.

AC2 stays open. A running third-party title cannot be arranged on the device: the server is the one foreground app, and launching another title suspends or ends the server, so there is no ssh session left to try the install from. The decision is covered by the host test (a running title is refused whatever the replace flag, even when installed), the running check by `vpk list` on the server's own package, and the refusal message by reading the code, but the refusal itself has not been seen on a device. Options for the user: accept that evidence and check AC2, or reword it to the host test plus the list check.

2026-11-18: added the long option on the user's request. `vpk install --reinstall FILE` is the explicit form of `-r`; both are accepted, and `vpk list` takes neither (usage error, exit 2). The refusal for an installed title, help, usage, README, AGENTS.md and docs/development.md now name --reinstall. Smoke checks added: --reinstall with no file exits 2, list rejects it, and a fixture package for the installed client title ROM000069 is refused with a message that names --reinstall (that fixture is never run with --reinstall, since it would replace the client with a package that is not runnable). Server text 2303234 (+48 over the previous build). Device: the first upload after the build failed with 'Connection closed' and a refused connection, so one smoke run went against the previous build and showed the old '-r' wording; after a retry the new build answers the new usage string. exec smoke on the new build: 117 pass 1 fail, the failure a doubled-operator chain check that returned ssh exit 255 (the intermittent connection failure noted on TASK-018 and TASK-025; the check passes on other runs). `vpk install --reinstall /ux0/downloads/megabonk-vita-smoke.vpk` replaced MBVK00001 ('Installing MBVK00001 over the installed copy', 'Installed MBVK00001'), /ux0/app/MBVK00001 still holds eboot.bin and sce_sys, and the stage directory was removed.

2026-11-18: AC2 reworded by the user to what can be shown: the running-title decision is covered by the host test and the running check by `vpk list` on the server's own package, since a running third-party title cannot be arranged on the device. Checked on that basis; the refusal message itself has not been seen on a device.
<!-- SECTION:NOTES:END -->

## Final Summary

<!-- SECTION:FINAL_SUMMARY:BEGIN -->
`vpk install` now installs a third-party VPK that is not running. The decision is the pure module src/vpk_policy.c, written test-first under ASan/UBSan against a literal table over thirteen title kinds, with nine broken versions each failing the test. The server's own title keeps the updater handoff; the updater title, NPXS system apps, a malformed title ID and a running title are refused; an already installed title is refused unless the command is `vpk install --reinstall FILE` (`-r` for short), which replaces the whole package and removes files the new one omits. Package contents are validated before the system installer is called (archive CRC, param.sfo TITLE_ID, eboot.bin with the SCE magic and at least 4 KiB), and every refusal leaves the staging directory clean. The third-party path stages in ux0:data/vitasshd-update/third, writes head.bin and calls vpk_promote() directly, with no updater. `vpk list` reports this app, another title, installed and running; staging yields to the poll loop per chunk. Verified on the Vita over ssh: the sister project's smoke package MBVK00001 installed in about five seconds and is present in /ux0/app, a second install was refused naming --reinstall, --reinstall replaced it, the own-title self-install still handed off cleanly after the refactor, and exec smoke ran 114 to 117 passes with the only failures being intermittent ssh exit 255 (cause unknown, also seen on other tasks). Caveats: a running third-party title cannot be arranged on the device (the server is the one foreground app), so AC2 was reworded to the host test plus `vpk list` reporting the server's own title as running, and the running-title refusal message has not been seen on a device; the promoter call blocks the server, and so every session, until the system finishes; replacing an installed title cannot be rolled back by the app. Server text grew 1,850 bytes.
<!-- SECTION:FINAL_SUMMARY:END -->
