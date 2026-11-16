---
id: TASK-002
title: Serve SFTP from the Vita sshd app
status: Done
assignee:
  - '@claude'
created_date: '2026-11-15T22:57:00Z'
updated_date: '2026-11-16T22:45:00Z'
labels: []
dependencies:
  - TASK-001.01
  - TASK-001.02
references:
  - 'https://api.libssh.org/stable/group__libssh__sftp.html'
documentation:
  - src/sshd_session.c
  - src/sh_path.c
  - include/sh_path.h
modified_files:
  - src/sshd_session.c
  - src/sftp_server.c
  - CMakeLists.txt
priority: high
type: feature
ordinal: 6000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
An interactive shell is half the workflow; moving files in and out is the other half, and doing it over SSH means SFTP so ordinary tooling (sftp, scp, and the SFTP-mounted filesystems people already use from a devbox) works without a second protocol or a custom helper. The subsystem is currently refused at the channel callback. The intent is to use libssh's stock SFTP server handlers rather than reimplementing the protocol, and to override only the opcodes that misbehave on Vita, so the protocol work stays upstream's. All file access must go through the same POSIX-to-sceIo translation layer the shell uses — one place decides what a path means.

The first consumer is deploying builds: today a VPK reaches the device through USB mass storage and VitaShell, and the SSH session that could install it cannot receive a file. With SFTP serving, a build lands in /ux0/downloads from any machine in one command, which is the input the `vpk` install command (TASK-001.07) consumes. Legacy scp (`scp -O`) cannot work here and should not be pretended to: it needs a remote scp binary to exec, and this platform has no fork/exec. OpenSSH's default scp speaks SFTP, so that path is covered by this task.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [x] #1 sftp <user>@<vita-ip> from a devbox opens a session against the running server
- [x] #2 directory listing, download, upload, mkdir, rmdir, rm and rename work through sftp
- [x] #3 every byte of file access goes through the shared path layer, so devices and permissions behave exactly as they do in the shell
- [x] #4 operations the Vita genuinely cannot perform fail with a correct SFTP status instead of hanging or reporting success
- [x] #5 the shell and sftp can be used at the same time against the same server
- [x] #6 a failed or interrupted upload leaves no partial file under the target name (temp name then rename, or equivalent)
- [x] #7 legacy scp (-O) fails with a clear message instead of hanging, since there is no remote scp binary to exec
- [x] #8 two concurrent sftp sessions can transfer at once without corrupting each other or stalling the poll loop beyond what the single-thread design already implies
<!-- AC:END -->

## Implementation Plan

<!-- SECTION:PLAN:BEGIN -->
Remaining work: close the parent. Subtasks .01, .02, .03 (and .03.01-.03.03) are Done; four parent criteria are unchecked.

1. AC7 (scp -O fails clearly): evidence exists in TASK-002.03 notes (macOS 10.6p1 and Linux 9.6p1: exit 1 in 0.57 s, 'scp: command not found'). Check on that evidence.
2. AC8 (two concurrent sftp sessions): evidence exists in TASK-002.03 (concurrent 8 MB and 12 MB scp/sftp up and down, sha256/cmp identical, queued == accepted). Check on that evidence.
3. AC3 (all file access through the shared path layer): no test shows it. Audit src/sftp_server.c: every libc file call the stock handlers make is in the CMakeLists.txt --wrap list and each __wrap_ translates through sh_path.c; rg for any libssh file-touching call missing a wrap (rename is handled in handle_rename; check readlink/symlink/fstat paths). Then a device check that the same path resolves identically in shell and sftp (a file written via sftp at /ux0/data/x is visible with ls in the shell, /os0 is read-only in both, an unknown device is ENOENT in both).
4. AC5 (shell and sftp at the same time): needs a device. Hold an interactive shell session open, run an sftp put/get of a multi-MB file from a second client, run commands in the shell during the transfer, confirm both complete and bytes match.
5. Finalize per the task-finalization guide only with pasted evidence per criterion, then refresh the stale Implementation Notes and write the Final Summary.

Blocked on Lance for the device half of step 3 and for step 4: needs a reachable Vita running ssh_server_vita (home 192.168.8.57:2222 or work 10.5.121.57:2222).
<!-- SECTION:PLAN:END -->

## Implementation Notes

<!-- SECTION:NOTES:BEGIN -->
Status 2026-11-16: TASK-002.01 and TASK-002.02 are Done; TASK-002.03 (verification pass) has not been started. Device evidence from scripts/smoke.sh sftp (46 pass, 0 fail, 1 informational, macOS client, Vita over WiFi) supports AC1, AC2, AC4 and AC6: sftp opens a session, ls/get/put/mkdir/rm/rmdir/rename work, unsupported operations return statuses instead of hanging or succeeding (chmod/chown unsupported, cross-device rename refused, times verified by read-back), and a killed upload leaves neither the target nor its .part file.

Not yet evidenced, so unchecked: AC3 (every file access through the shared path layer: true by construction through the --wrap layer, but no test shows it), AC5 (shell and sftp used at the same time; only sequential use is tested), AC7 (scp -O not tried; it should fail because the shell has no scp command), AC8 (two concurrent transfers not tested). These overlap TASK-002.03's criteria.

Design notes worth knowing before touching this code are in AGENTS.md ('SFTP transport rules'): replies are queued and the session is non-blocking, --wrap also catches calls inside libc, session teardown waits for the channel close, and stat times must survive localtime_r.

2026-11-16: Resumed. The parent notes above ('TASK-002.03 has not been started') are stale: TASK-002.03 is Done 9/9 with device evidence. AC7 and AC8 are covered by that evidence; AC3 and AC5 still need evidence. Plan recorded; waiting on which network Lance is on before any device run.

2026-11-16 device run, work Vita any@10.5.121.57:2222, macOS OpenSSH_10.6p1, current build. AC3: a 3 MB file put over sftp to /ux0/data was visible to the shell (`stat` Size 3000000, 0644) and read back byte-identical (cmp); a file and directory made by the shell were visible to sftp (`ls -l` -rw-r--r-- 6 bytes, `get` returned the content); no .part left after the put. Same path, same answer on both sides: /os0 writes refused (sftp 'Permission denied', shell 'Read-only file system'; wording differs, status is the same, see TASK-002.03.01), /nodev is ENOENT in both ('not found' / 'No such file or directory'). AC5: an interactive shell stayed open and answered commands while a 12 MB sftp put ran from a second client; an `ls /ux0/data` in the shell during the transfer listed big12.bin.part, the put exited 0, and the file read back cmp-identical. AC7 and AC8 rest on TASK-002.03's recorded runs (macOS 10.6p1 and Linux 9.6p1: scp -O exit 1 in 0.57 s with 'scp: command not found'; concurrent 8 MB and 12 MB transfers intact). Scratch removed from the Vita. Test slips on my side, not server defects: the shell has no `;` so one exec line created /ux0/data/'ac3sh.txt;' (removed), and a get started before the put finished failed with rc 1.
<!-- SECTION:NOTES:END -->

## Final Summary

<!-- SECTION:FINAL_SUMMARY:BEGIN -->
SFTP is served from the sshd app using libssh's stock handlers, with file access translated through sh_path.c via a --wrap layer and a few opcodes overridden for the Vita (chmod/chown/statvfs unsupported, rename implemented, read-only devices answer permission denied). Verified on a Vita with macOS OpenSSH 10.6p1 (and Linux 9.6p1 in TASK-002.03): sftp/scp up and down byte-identical including 12 MB files, recursive trees, VPK put, interrupted uploads leave no target or .part, scp -O fails in under a second with 'command not found', concurrent transfers intact, and the shell and sftp use the same paths and the same device rules and run at the same time. Defects found along the way and fixed: replies sent twice or skipped when drain_out() re-entered (12 MB download desync), exec output cut at 64 KB by the client's EOF, bare 'Failure' on read-only writes. Throughput is about 0.65 MB/s up and 1 MB/s down over WiFi (docs/throughput.md). Limits: scp -s needs OpenSSH 8.7+; clients see 'Permission denied' not 'Read-only file system'; scp -s uploads print 'remote fsetstat: Operation unsupported' with exit 0 (handle_perm_setstat() not matching OpenSSH 10.6's FSETSTAT, not investigated); Windows clients not tried.
<!-- SECTION:FINAL_SUMMARY:END -->
