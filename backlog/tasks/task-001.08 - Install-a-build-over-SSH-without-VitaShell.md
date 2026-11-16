---
id: TASK-001.08
title: Give the shell a `vpk` command that installs builds over SSH
status: To Do
assignee: []
created_date: '2026-11-16T04:30:00Z'
updated_date: '2026-11-16T04:33:00Z'
labels:
  - deploy
  - server
  - shell
dependencies:
  - TASK-001.07
references:
  - TASK-001.07
documentation:
  - 'https://docs.alpinelinux.org/user-handbook/0.1a/Working/apk.html'
modified_files:
  - src/sshd_builtins.c
  - CMakeLists.txt
parent_task_id: TASK-001
priority: low
type: enhancement
ordinal: 20000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
The deploy loop today is: task build, USB mass storage, VitaShell drag, unmount, relaunch. The SSH server can already receive files once TASK-001.07 lands; what is missing is turning a VPK on disk into the running app's next launch.

The command is named `vpk` — apk-shaped (`vpk install FILE`, `vpk list FILE`) — rather than squatting on `install`, which coreutils owns with different semantics (copy plus mode bits) and which a future builtin should still be able to use. Nothing in this shell reserves command names: dispatch is a lookup of argv[0] in the builtin table and nothing else — there are not even POSIX reserved words here, since the shell has no grammar, only words.

The apparent chicken-and-egg — the shell asked to install the new build is inside the app being replaced — is not one: installing writes files, launching executes them, and the Vita loads the app from storage at launch, so the running instance can lay down the new binary and simply not be it. Vita homebrew already does this (VitaShell's updater installs its own new VPK over the running instance). The platform's patch overlay (ux0:patch/<TITLEID> loaded over ux0:app/<TITLEID> at launch) is the fallback if the running app's own files ever turn out to be locked: write the new build there and the next launch uses it, with deletion of the patch dir as the rollback.

`vpk` unpacks a VPK (a zip; zlib is already linked) into the app's own directory, refuses anything that is not this app's own title ID or whose eboot.bin is missing/implausible, and ends by telling the user to relaunch — the app cannot restart itself, and should not try. Verify the self-overwrite assumption and the patch fallback on hardware before committing to either.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 `vpk install /ux0/downloads/ssh_server_vita.vpk` unpacks the package and the app relaunches into the new build, verified on hardware by a version string or behavior change.
- [ ] #2 `vpk list FILE` prints a package's title ID and the files it carries, without installing anything.
- [ ] #3 `vpk` refuses a package whose title ID is not the server's own, and one with no plausible eboot.bin, each with a clear message.
- [ ] #4 Installing replaces only the files the package carries: a file already in the app directory that the package does not contain is left untouched (apk's overwrite rule, minus the .apk-new dance — the patch directory is this install's rollback instead).
- [ ] #5 An install never changes the data directory that holds the host key and authorized_keys (ux0:/data/vitasshd): credentials survive every install, verified on hardware. The package installs to ux0:app/<TITLEID>, so this holds by construction — the AC pins it against a future refactor that installs elsewhere.
- [ ] #6 A failed or interrupted install (client disconnect mid-unpack, full memory card) leaves the currently installed app launchable and unchanged — the old build still boots after deleting any partial artifacts.
- [ ] #7 After a successful install, quitting and relaunching does not resurrect the old build.
- [ ] #8 If the running app's directory turns out to be locked for overwrite, the patch-overlay route (ux0:patch/<TITLEID>) is implemented instead, with deleting the patch directory as the documented rollback to the installed base build.
- [ ] #9 `vpk` is listed in `help` and found by `which vpk`.
<!-- AC:END -->
