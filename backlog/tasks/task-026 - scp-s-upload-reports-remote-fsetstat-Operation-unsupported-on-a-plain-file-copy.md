---
id: TASK-026
title: >-
  scp -s upload reports "remote fsetstat: Operation unsupported" on a plain file
  copy
status: Done
assignee:
  - claude
created_date: '2026-11-17T16:44:00Z'
updated_date: '2026-11-17T17:14:00Z'
labels:
  - sftp
  - scp
dependencies: []
references:
  - AGENTS.md
  - src/sftp_server.c
  - scripts/smoke.sh
modified_files:
  - src/sftp_server.c
  - scripts/smoke.sh
  - AGENTS.md
priority: medium
type: bug
ordinal: 57000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
Observed 2026-11-17 on the work Vita while uploading two files with `scp -s -o BatchMode=yes <file> any@<host>:/ux0/data/<name>`: both transfers completed and the files are on the card with the right sizes, but scp printed `scp: remote fsetstat: Operation unsupported` once per file. AGENTS.md says `handle_perm_setstat()` answers OK to a SETSTAT/FSETSTAT that carries only permission bits and asks for exactly the mode already reported (0644 file), because that is what scp sends after every upload. Either that case is not being hit (the request may carry more than permission bits, or a different mode than the one reported) or the path is not reached for FSETSTAT on a handle. Find out which, and fix the server or correct the AGENTS.md claim. Check scripts/smoke.sh sftp mode, which is supposed to cover the scp upload case, to see why it did not catch this.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [x] #1 The cause is identified from the request actually sent (attributes and mode in the FSETSTAT packet), not guessed.
- [x] #2 A plain `scp -s` upload to /ux0/data exits 0 with no fsetstat warning, or AGENTS.md is corrected to say why the warning is expected.
- [x] #3 scripts/smoke.sh sftp mode fails on the warning before the fix and passes after it.
<!-- AC:END -->

## Implementation Plan

<!-- SECTION:PLAN:BEGIN -->
1. Reproduce on the work Vita (identified from the Mac's subnet per CLAUDE.local.md): scp -s upload of files with source modes 0644, 0600, 0755, then read sshd.log to get the attributes actually sent in the FSETSTAT (AC1).
2. Write a failing check in scripts/smoke.sh sftp mode first: do not pass -q, capture scp stderr, fail on any `fsetstat` text (AC3). Run it to confirm it fails.
3. Fix per the evidence: either accept in handle_perm_setstat() what scp sends, or correct the AGENTS.md claim (AC2).
4. Rebuild with `task --force build`, rerun smoke sftp mode, confirm pass.
<!-- SECTION:PLAN:END -->

## Implementation Notes

<!-- SECTION:NOTES:BEGIN -->
Cause, read from the server log on the work Vita: the packet is type 10 (FSETSTAT) with flags 0x1 (SSH_FILEXFER_ATTR_SIZE), a 64-bit size and no permission bits, regardless of the source file's mode (0644, 0600 and 0755 sources all warned). handle_perm_setstat() never matched it, so the stock process_unsupported answered. The AGENTS.md claim that scp sends permission bits after an upload was wrong for OpenSSH 10.6 scp -s.

Why smoke.sh missed it: scp exits 0 on this warning, and the check passed -q and never read stderr. It now drops -q and fails on any 'fsetstat' in scp's stderr; it failed before the fix and passes after.

Fix: handle_size_fsetstat() answers OK when the size equals the open file's size. The fd comes from libssh's private struct sftp_handle (head mirrored from pinned 0.12.2) and is trusted only if it is one of the server's own slots.
<!-- SECTION:NOTES:END -->

## Final Summary

<!-- SECTION:FINAL_SUMMARY:BEGIN -->
Plain `scp -s` uploads no longer print `remote fsetstat: Operation unsupported`. The request scp sends is an FSETSTAT with only the size attribute, not the permission bits AGENTS.md claimed, so `handle_perm_setstat()` never matched it. `handle_size_fsetstat()` in `src/sftp_server.c` now answers OK when the size equals the open file's size, and anything else still answers unsupported. `scripts/smoke.sh` sftp mode now grades scp's stderr and failed on the warning before the fix; after the fix it ran 83 pass, 0 fail, 1 skip on the work Vita. AGENTS.md is corrected, including the libssh private-struct dependency that a libssh bump must recheck.
<!-- SECTION:FINAL_SUMMARY:END -->
