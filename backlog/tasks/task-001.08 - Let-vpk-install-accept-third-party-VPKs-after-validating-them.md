---
id: TASK-001.08
title: Let vpk install accept third-party VPKs after validating them
status: In Progress
assignee:
  - Claude
created_date: '2026-11-18T07:14:00Z'
updated_date: '2026-11-18T07:37:00Z'
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
- [ ] #1 `vpk install FILE` installs a valid VPK for a title that is not the server's own and is not running, and the title is present in the installed apps afterwards, verified on a device with the sister project's Vita smoke package (title MBVK00001, staged at ux0:downloads/megabonk-vita-smoke.vpk), which stays installed for that project's own hardware checks.
- [ ] #2 A package whose title is running is refused with a readable message naming the title, nothing is written for it, and no running app is closed or destroyed by the command.
- [ ] #3 The package is validated before the promoter is called: archive integrity, a well-formed param.sfo with a TITLE_ID, and an eboot.bin that starts with the SCE magic and is not implausibly small; each failure prints its reason and leaves the device unchanged and the stage clean.
- [ ] #4 Installing over a title that is already installed is refused unless the command is given an explicit option, and the refusal says the whole package would be replaced and files omitted from the new one removed.
- [ ] #5 The server's own title ROM000070 and the updater title ROM000072 are never installed through this path: the own title keeps using the updater handoff, and the updater title is refused.
- [ ] #6 `vpk list` still reports whether a package is this app, another title, or a title that is already installed.
- [ ] #7 The validation decisions have a host test written first, covering accepted and refused packages including the own, updater, running and already-installed cases.
- [ ] #8 README, help, docs/development.md and AGENTS.md state what `vpk install` accepts; the wording that says it handles any VPK is corrected whichever way this task lands.
<!-- AC:END -->

## Definition of Done
<!-- DOD:BEGIN -->
- [ ] #1 task build produces both VPKs
- [ ] #2 Verified on a device over a real ssh session
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
<!-- SECTION:PLAN:END -->

## Implementation Notes

<!-- SECTION:NOTES:BEGIN -->
2026-11-18: test fixture chosen by the user for AC1: the sister project's Vita smoke package, title MBVK00001, already on the device at ux0:downloads/megabonk-vita-smoke.vpk (46.3 KB, copied with this repo's sftp server). `vpk list` on the current build reads it correctly: 6 files (param.sfo, an 48442-byte eboot.bin, icon0.png, livearea bg.png, startup.png and template.xml), 52122 bytes, reported as 'not this app'. This is the real case behind the task: the sister project's TASK-084.01 recorded that this server's `vpk install` rejected exactly this package because it only accepts the server's own title, leaving a VitaShell install as the only route. Installing it is what that project wants anyway, so AC1's 'removed again' was dropped: the package stays installed. Not yet installed through this server.
<!-- SECTION:NOTES:END -->
