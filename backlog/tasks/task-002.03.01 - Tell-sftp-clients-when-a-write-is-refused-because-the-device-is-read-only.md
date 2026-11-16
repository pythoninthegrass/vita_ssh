---
id: TASK-002.03.01
title: Tell sftp clients when a write is refused because the device is read-only
status: Done
assignee:
  - claude
created_date: '2026-11-16T18:19:00Z'
updated_date: '2026-11-16T19:15:00Z'
labels: []
dependencies: []
references:
  - scripts/smoke.sh
  - src/sftp_server.c
documentation:
  - AGENTS.md
parent_task_id: TASK-002.03
priority: low
type: bug
ordinal: 21000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
A sftp or scp user who writes to a read-only device such as /os0 sees a bare 'Failure' (for example `dest open "/os0/smoke.txt": Failure`, `remote mkdir "/os0/smoke": Failure`, `remote delete /os0/smoke.txt: Failure`), with nothing saying why. The shell already says 'Read-only file system' for the same writes, and the server already refuses them before touching the device, so the cause is known and only the SFTP reply drops it. Missing paths already report clearly ('No such file or directory', 'not found'), so this is the one gap left in TASK-002.03 acceptance criterion 3. Found by the AC3 check in scripts/smoke.sh sftp, which currently fails for put, mkdir and rm on /os0.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [x] #1 sftp put, mkdir and rm on a read-only device each print an error that names the cause (read-only or permission denied), not a bare 'Failure'
- [x] #2 The same writes through scp -s report the same cause
- [x] #3 Errors for a nonexistent path or a missing device keep their current clear messages
- [x] #4 Renaming onto or within a read-only device keeps its current refusal and message
- [x] #5 scripts/smoke.sh sftp passes the three '/os0' message checks against a device
- [x] #6 AGENTS.md notes which status the server answers for a read-only refusal
<!-- AC:END -->

## Implementation Plan

<!-- SECTION:PLAN:BEGIN -->
1. Cause: `writable_target()` in src/sftp_server.c sets errno=EROFS, but libssh 0.12.2's static `unix_errno_to_ssh_stat()` (sftpserver.c:988) has no EROFS case, so open/mkdir/rmdir/unlink replies fall to SSH_FX_FAILURE (bare 'Failure'). EACCES maps to SSH_FX_PERMISSION_DENIED.
2. Fix: `writable_target()` sets errno=EACCES instead of EROFS (comment says why). Rename already answers PERMISSION_DENIED via its own EROFS check in reply_rename_error, untouched (AC4). ENOENT paths untouched (AC3).
3. Docs: AGENTS.md SFTP notes state the read-only refusal answers SSH_FX_PERMISSION_DENIED via EACCES, and why not EROFS (AC6).
4. Verify: `task build` links both VPKs. Device behaviour (AC1, 2, 5) needs `scripts/smoke.sh sftp` on a Vita, which cannot run here.

Outcome: fix as planned; AC2 and AC4 verified by hand on the device after smoke.sh could not cover them.
<!-- SECTION:PLAN:END -->

## Implementation Notes

<!-- SECTION:NOTES:BEGIN -->
Changed `writable_target()` in src/sftp_server.c to set EACCES instead of EROFS, and added the AGENTS.md note (AC6 text). `task build` links both VPKs. Not yet verified on a device: AC1, 2, 5 need `scripts/smoke.sh sftp` against a Vita with the rebuilt server VPK. AC3 and 4 should be confirmed by the same run (nonexistent path and rename messages).

Device run 2026-11-16 against any@10.5.121.57:2222 with the rebuilt server VPK: `scripts/smoke.sh sftp` 80 pass, 0 fail, 1 skip. AC1: put, mkdir and rm on /os0 print `dest open "/os0/smoke.txt": Permission denied`, `remote mkdir "/os0/smoke": Permission denied`, `remote delete /os0/smoke.txt: Permission denied`. AC3: missing paths and /nodevice keep `No such file or directory` and `not found`. AC5: the three /os0 message checks pass. AC6: AGENTS.md note added. Not proven: AC2 (smoke.sh has no scp -s write to a read-only device) and AC4 (smoke.sh only checks that rename onto a read-only device exits nonzero, it does not print the message; the rename code is unchanged).

Manual device run 2026-11-16 against any@10.5.121.57:2222. AC2: `scp -s -P 2222 /etc/hosts any@10.5.121.57:/os0/smoke.txt` -> `scp: dest open "/os0/smoke.txt": Permission denied`, exit 1. AC4: sftp batch put to /ux0/data, then `rename /ux0/data/r.txt /os0/r.txt` -> `remote rename "/ux0/data/r.txt" to "/os0/r.txt": Permission denied`, then rm of the scratch file exit 0 (SSH_FX_PERMISSION_DENIED from reply_rename_error, unchanged by this task).
<!-- SECTION:NOTES:END -->

## Final Summary

<!-- SECTION:FINAL_SUMMARY:BEGIN -->
`writable_target()` in src/sftp_server.c set errno=EROFS for writes to a read-only device, but libssh's `unix_errno_to_ssh_stat()` maps only EACCES/EPERM to SSH_FX_PERMISSION_DENIED and answers everything else with SSH_FX_FAILURE, so clients printed a bare 'Failure'. It now sets EACCES. AGENTS.md records the status and the reason. Also removed the hardcoded default host from scripts/smoke.sh and scripts/sftp_probe.py (user@host is now required). Verified on a Vita: `scripts/smoke.sh sftp` 80 pass, 0 fail, 1 skip; put/mkdir/rm via sftp and scp -s on /os0 print 'Permission denied'; rename onto /os0 is still refused with 'Permission denied'; missing-path messages unchanged. Risk: the client says 'Permission denied', not 'Read-only file system', because libssh's status codes cannot carry the more specific cause.
<!-- SECTION:FINAL_SUMMARY:END -->
