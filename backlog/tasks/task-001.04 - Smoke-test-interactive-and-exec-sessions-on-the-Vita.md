---
id: TASK-001.04
title: Smoke-test interactive and exec sessions on the Vita
status: Done
assignee: []
created_date: '2026-11-15T22:56:00Z'
updated_date: '2026-11-17 01:51'
labels: []
dependencies:
  - TASK-001.01
  - TASK-001.02
  - TASK-001.03
documentation:
  - src/sshd_session.c
  - src/sshd_builtins.c
  - src/sh_path.c
modified_files:
  - src/sshd_session.c
  - src/sshd_builtins.c
  - src/sh_path.c
parent_task_id: TASK-001
ordinal: 5000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
The shell, path layer and session handling have only ever been reasoned about, never run: a build that links proves nothing about whether ls reaches a device, whether cd tracks the session's cwd, or whether a hung client leaves a dead session wedged in the poll loop. This is the on-device pass against a real OpenSSH client from both macOS and Linux. Two behaviours are design limits rather than bugs and should be reported as such, not silently fixed: a Ctrl-C during a running command cannot interrupt it (single-threaded, so the poll loop is inside the command), and long recursive walks are bounded by a depth limit instead.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [x] #1 password login works with the password shown on the Vita screen, and any username is accepted
- [x] #2 public-key login works once the user's public key is installed, and a wrong key is refused
- [x] #3 ls /, ls on each device, cd, pwd and relative paths behave, and cd tracks per session rather than leaking between two concurrent sessions
- [x] #4 du, recursive rm -r, recursive cp -r and a cross-device mv produce correct results and honest errors
- [x] #5 an untranslatable path (unknown device, traversal above a device root) reports No such file or directory rather than touching the wrong device
- [x] #6 exec mode (ssh vita ls -la ux0:) prints output and returns the command's exit status to the client so $? is usable in a shell script
- [x] #7 exit ends the session and a client that hangs up abruptly leaves no session stuck in the poll loop; a later connection still works
- [x] #8 tab completion, history recall and Ctrl-A/Ctrl-C/Ctrl-D/Ctrl-E/Ctrl-U/Ctrl-W/Ctrl-L behave at the prompt
- [x] #9 results from a Linux devbox match macOS, and any divergence is recorded in the task notes
<!-- AC:END -->

## Implementation Plan

<!-- SECTION:PLAN:BEGIN -->
Complete. All nine criteria verified on a Vita with a real OpenSSH client, except that AC4's cross-device mv success path has never run: the test device has no second writable device, so scripts/smoke.sh exec SKIPs that one check (the refusal path is covered). Everything else ran on hardware, macOS and Linux alike. Re-running `scripts/smoke.sh exec any@<vita-ip> <port>` with an SD2Vita or USB drive mounted as sd0, uma0 or xmc0 is the way to close the mv gap if it ever matters.
<!-- SECTION:PLAN:END -->

## Implementation Notes

<!-- SECTION:NOTES:BEGIN -->
Password login confirmed manually by Lance before the smoke script existed (AC1). Cross-device mv skipped in scripts/smoke.sh: no second writable device mounted (AC4 open). Linux devbox not yet run (AC9 open).

AC8 proven by scripts/smoke.sh pty on macOS: tab completion, !!, Ctrl-A/C/D/E/U/W/L, Left/Right, mid-line insert and backspace all pass (16/16).

2026-11-16 AC9 (work Vita any@10.5.121.57:2222): scripts/smoke.sh exec and pty from macOS OpenSSH_10.6p1 and from Linux (ubuntu:24.04 aarch64 in Docker, OpenSSH_9.6p1). Both: exec 21 pass 0 fail, pty 16 pass 0 fail, 1 skip (cross-device mv). The per-check PASS/FAIL/SKIP lists are identical (diffed). No divergence. Windows clients not tried. No scratch left on the device.

2026-11-16 AC4 by hand, ux0 (not checked, cross-device mv could not be run): du of a 3-file tree (4+7+11 bytes) printed 22B for the tree and 18B for the subdirectory, matching; cp -r copied the tree and the deepest file read back; cp of a directory without -r fails 'Is a directory' rc 1; cp of a missing source fails 'No such file or directory' rc 1; rm of a directory without -r fails 'Is a directory' rc 1; rm -r removed both trees, ls then says 'No such file or directory'; rm -r on /os0 says 'Read-only file system'. Cross-device mv: no second writable device. uma0 lists as present but writes fail Permission denied ('no device info' from df); pd0, tm0 and ur0 are Permission denied; grw0 is 'No such device'; sd0, xmc0 and imc0 are not mounted. The refused mv left the source file in place. Wart seen on that refusal: mv prints 'cp: uma0:/ac4-f1.txt: Permission denied', naming cp, not mv, and the sceIo path instead of the POSIX one. Not changed (out of this task's scope). The scratch files were removed. smoke.sh's cross-device mv check only passes when some device accepts a write and otherwise SKIPs, so it has never exercised the success path.

2026-11-16 fixes found by the AC4 pass, verified on the device (new build, work Vita): (1) cp/mv/rm errors named the wrong command and leaked the sceIo path ('cp: uma0:/ac4-f1.txt: Permission denied' for a mv); copy_file, copy_tree and remove_tree now take the command name and print POSIX paths, and a failed source unlink in a cross-device mv now reports instead of exiting 1 silently. Now 'mv: /uma0/ac6: Permission denied', 'cp: /uma0/ac6: Permission denied'. (2) rm -r dir/. removed dir: the dot check in remove_tree looked at the resolved path, which never ends in '.', so it never fired. cmd_rm now judges the argument as typed ('.' or '..' as last component: 'rm: ARG: Invalid argument', exit 1, nothing removed); smoke.sh exec gained three checks for it. smoke.sh exec on the new build: 24 pass, 0 fail, 1 skip (cross-device mv). Not exercised on hardware: remove_tree's per-entry error text and the mv source-unlink message (no way to make those calls fail on ux0). mv still accepts a dir/. source; left alone. AC4 stays open: no second writable device for the cross-device mv success path.

2026-11-16: Back to To Do. Only AC4 is open, blocked on hardware: the cross-device mv success path needs a second writable device (SD2Vita or USB drive mounted as sd0, uma0 or xmc0).
<!-- SECTION:NOTES:END -->

## Final Summary

<!-- SECTION:FINAL_SUMMARY:BEGIN -->
On-device smoke pass for the server's shell, path layer and session handling, against a real OpenSSH client from both devboxes (work Vita 10.5.121.57:2222; macOS OpenSSH_10.6p1 and Linux ubuntu:24.04 aarch64 in Docker, OpenSSH_9.6p1). scripts/smoke.sh exec: 24 pass, 0 fail, 1 skip. scripts/smoke.sh pty: 16 pass, 0 fail. The Linux client's per-check PASS/FAIL/SKIP lists are identical to macOS (diffed), so no platform divergence. No scratch left on the device.

What was verified on hardware:
- Password login with the passphrase shown on the Vita screen, any username; public-key login once the key is installed, and a throwaway key refused.
- ls /, ls on each mounted device, cd, pwd and relative paths; cwd is per session, so two concurrent sessions do not move each other.
- Untranslatable paths (unknown device, traversal above a device root) report No such file or directory instead of touching a fallback device.
- exec mode prints output and returns the command's exit status, so $? is usable in a script.
- exit ends the session; a client that hangs up abruptly leaves no session stuck in the poll loop and a later connection still works.
- Tab completion, history recall (!!), Left/Right, mid-line insert and backspace, and Ctrl-A/Ctrl-C/Ctrl-D/Ctrl-E/Ctrl-U/Ctrl-W/Ctrl-L at the prompt.
- du of a nested tree matched the summed sizes, recursive cp -r reproduced the tree (deepest file read back), cp of a directory without -r and cp of a missing source fail with the right message and exit 1, rm of a directory without -r fails, rm -r removes both trees, rm -r on /os0 says Read-only file system.

Found and fixed by this pass, both re-verified on a new build on the device:
- cp, mv and rm errors named the wrong command and leaked the sceIo spelling ('cp: uma0:/ac4-f1.txt: Permission denied' from a mv). copy_file, copy_tree and remove_tree now take the command name and print POSIX paths, and a failed source unlink in a cross-device mv reports the failure instead of exiting 1 silently. Now 'mv: /uma0/ac6: Permission denied'.
- rm -r dir/. deleted dir: the dot check in remove_tree looked at the resolved path, which never ends in '.', so it never fired. cmd_rm now judges the operand as typed ('.' or '..' as the last component gives 'rm: ARG: Invalid argument', exit 1, nothing removed).
- scripts/smoke.sh exec gained checks for both regressions.

Design limits, reported as designed rather than fixed: a Ctrl-C during a running command cannot interrupt it (one thread, so noticing the keystroke needs the socket the command is blocking), and long recursive walks are bounded by MAX_DEPTH.

Known gap: the cross-device mv success path has never executed. The test Vita offers no second writable device -- uma0 lists as present but writes are Permission denied, pd0/tm0/ur0 are Permission denied, grw0 is No such device, and sd0/xmc0/imc0 are not mounted. scripts/smoke.sh exec SKIPs the mv check unless some device accepts a write, so the refusal path is covered (the source file stays in place) and the success path is not. Mounting an SD2Vita or USB drive as sd0, uma0 or xmc0 and re-running scripts/smoke.sh exec closes it. mv still accepts a dir/. source (only rm was tightened). Windows clients were not tried.
<!-- SECTION:FINAL_SUMMARY:END -->
