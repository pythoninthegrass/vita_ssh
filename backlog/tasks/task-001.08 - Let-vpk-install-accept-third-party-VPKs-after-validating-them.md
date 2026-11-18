---
id: TASK-001.08
title: Let vpk install accept third-party VPKs after validating them
status: To Do
assignee: []
created_date: '2026-11-18T07:14:00Z'
updated_date: '2026-11-18T07:33:00Z'
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

## Implementation Notes

<!-- SECTION:NOTES:BEGIN -->
2026-11-18: test fixture chosen by the user for AC1: the sister project's Vita smoke package, title MBVK00001, already on the device at ux0:downloads/megabonk-vita-smoke.vpk (46.3 KB, copied with this repo's sftp server). `vpk list` on the current build reads it correctly: 6 files (param.sfo, an 48442-byte eboot.bin, icon0.png, livearea bg.png, startup.png and template.xml), 52122 bytes, reported as 'not this app'. This is the real case behind the task: the sister project's TASK-084.01 recorded that this server's `vpk install` rejected exactly this package because it only accepts the server's own title, leaving a VitaShell install as the only route. Installing it is what that project wants anyway, so AC1's 'removed again' was dropped: the package stays installed. Not yet installed through this server.
<!-- SECTION:NOTES:END -->
