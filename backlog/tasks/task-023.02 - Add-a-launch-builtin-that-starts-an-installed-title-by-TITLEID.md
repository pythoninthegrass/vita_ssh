---
id: TASK-023.02
title: Add a launch builtin that starts an installed title by TITLEID
status: To Do
assignee: []
created_date: '2026-11-17 05:10'
labels:
  - shell
  - builtin
  - appmgr
dependencies: []
references:
  - src/sshd_vpk.c
  - src/sshd_updater.c
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
ordinal: 52000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
Add 'launch <TITLEID>' to the server shell, so a build can be started from an ssh session instead of by hand on the device.

API. One call does it: sceAppMgrLaunchAppByUri(0xFFFFF, "psgm:play?titleid=<TID>"). Do not invent a different spelling -- this exact call with these exact flags is already working in this app at src/sshd_vpk.c:459 (launches the updater) and src/sshd_updater.c:44 (relaunches the server even on failure). SceAppMgr_stub is already in the server's link list (CMakeLists.txt:214); add no libraries.

Shape. static int cmd_launch(sh_session_t *s, int argc, char **argv) in src/sshd_builtins.c, plus a row in builtins[] at the bottom of the file. Alphabetically that is between "history" and "ls"; the table order is hand-maintained and 'help' prints it as-is, so verify the sort after inserting.

Arguments. A Vita title id is nine characters from [A-Z0-9]. Reject anything else before touching an API: 'launch: <arg>: not a title id'. Refuse this app's own title id with a message pointing at 'vpk install' -- launching the server while it is serving is the updater handoff's job, not this command's, and it would sever the session that asked.

Pre-flight. Call scePromoterUtilityCheckExist(titleid, &res) (psp2/promoterutil.h:128) so an uninstalled title reads 'launch: <tid>: not installed' rather than an opaque appmgr code; Init/Exit it the way src/vpk_promote.c does. If the pre-flight call itself fails, do NOT refuse the launch -- fall through and report the launch result, so a broken probe never becomes the user's problem.

The hazard that makes this task non-trivial: launching destroys this app, so anything still queued to the client is lost with it. AGENTS.md's rule for exec commands applies -- queue exit status and EOF, then wait for libssh's buffer to empty before acting, because the session is freed right after. 'vpk install' already solves the same problem when it hands off to the updater (it closes the command's channel first, see src/sshd_vpk.c); reuse that shape rather than inventing one. Print the whole line before launching, including that launching ends every session, not just this one.

appmgr returns 0x80...... style codes that newlib's strerror() cannot name. Print the number beside the text, the way cmd_vcmd prints its socket error. AGENTS.md has this rule generally; do not ship a message that reads "No data" or "Unknown error".
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 launch <TID> for an installed title brings it to the foreground, verified on a device.
- [ ] #2 The client receives the complete message and a clean exit status -- not a truncated line, not 255 -- verified on a device.
- [ ] #3 An uninstalled title id prints 'launch: <tid>: not installed' rather than a bare hex code, verified on a device.
- [ ] #4 A malformed id and this app's own title id are refused before any API call.
- [ ] #5 Any appmgr failure code is printed in hex beside its strerror() text.
- [ ] #6 help lists launch with its usage, which launch reports shell builtin, builtins[] stays alphabetically sorted, and task build is clean.
<!-- AC:END -->
