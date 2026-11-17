---
id: TASK-023.04
title: Add a quit builtin that destroys a running title by TITLEID
status: To Do
assignee: []
created_date: '2026-11-17 05:10'
labels:
  - shell
  - builtin
  - appmgr
dependencies: []
references:
  - src/sshd_updater.c
  - include/vpk_update.h
  - docs/vcmd.md
  - 'https://github.com/devnoname120/vitacompanion'
documentation:
  - AGENTS.md
  - docs/vcmd.md
modified_files:
  - src/sshd_builtins.c
  - README.md
  - docs/vcmd.md
parent_task_id: TASK-023
priority: medium
ordinal: 54000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
Add 'quit <TITLEID> | all' to the server shell, so a runaway or hung title can be killed over ssh.

API. sceAppMgrGetIdByName(&pid, name) then sceAppMgrDestroyAppByName(name). The working precedent is src/sshd_updater.c:24-32, which is deliberately tolerant: if get-id fails it treats the title as not running and continues. Keep that tolerance -- 'quit' for something that is not running should say so and return 0, not fail. Both calls are user-mode and SceAppMgr_stub is already linked (CMakeLists.txt:214); add no libraries.

Two refusals, both load-bearing.
  1. This server's own title id. Destroying it mid-command severs every session with no cleanup. The sanctioned targeted shutdown of this title is the updater's (src/sshd_updater.c does exactly this via sceAppMgrGetIdByName/DestroyAppByName); this command should refuse and say so.
  2. The updater's title id while an update is staged. Killing the updater between staging and promoting leaves the stage under ux0/data/vitasshd-update and a ready marker behind; include/vpk_update.h is the path contract that says what is on disk. Check the marker and refuse.

'all' is a different API and a blunter one: sceAppMgrDestroyOtherApp() destroys every other app, including VitaShell and the updater, with no chance to pick. Because of that, require an explicit second word ('quit all confirm') and print what it destroys before doing it. Verify on a device that it leaves THIS app running -- if DestroyOtherApp takes the server too, 'all' is unusable and that finding must be reported rather than shipped.

A listing would be nicer than a blunt instrument. sceAppMgrGetRunningAppIdListForShell() exists in psp2/appmgr.h, but the ForShell suffix suggests it wants shell privileges; whether an unsigned homebrew app may call it is UNVERIFIED. Probe it once on a device. If it is refused, implement 'all' without a listing and say so plainly in the usage text and in docs/vcmd.md. Do not fake a listing and do not silently drop 'all'.

Print every appmgr failure as hex beside strerror() text -- these are 0x80...... codes newlib cannot name, and cmd_vcmd already sets the pattern in this file.

Add the row to builtins[] in alphabetical position (between 'pwd' and 'rm').
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 quit <TID> stops a title the user launched, verified on a device, and reports 'not running' with status 0 for a title that is not running.
- [ ] #2 Quitting this server's own title id is refused with a message that points at the sanctioned path.
- [ ] #3 Quitting the updater's title id is refused while an update is staged.
- [ ] #4 quit all requires an explicit confirm word and states what it destroys before destroying it.
- [ ] #5 On a device, quit all leaves the server itself running and reachable; if it does not, that is reported and 'all' is not shipped as silent.
- [ ] #6 sceAppMgrGetRunningAppIdListForShell is probed once and the result is recorded in docs/vcmd.md either way.
- [ ] #7 help lists quit with its usage, which quit reports shell builtin, builtins[] stays alphabetically sorted, and task build is clean.
<!-- AC:END -->
