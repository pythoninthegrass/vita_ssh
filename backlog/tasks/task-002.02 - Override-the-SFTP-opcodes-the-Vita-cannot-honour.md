---
id: TASK-002.02
title: Override the SFTP opcodes the Vita cannot honour
status: In Progress
assignee:
  - '@claude'
created_date: '2026-11-15T22:57:00Z'
updated_date: '2026-11-16T06:40:00Z'
labels: []
dependencies:
  - TASK-002.01
documentation:
  - src/sshd_session.c
  - src/sh_path.c
modified_files:
  - src/sftp_server.c
  - include/sftp_server.h
  - CMakeLists.txt
  - AGENTS.md
parent_task_id: TASK-002
type: task
ordinal: 8000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
Stock handlers answer opcodes the Vita cannot back, and the wrong answer here is worse than an error: a client that is told chmod/chown/utimes succeeded will believe permissions and timestamps it did not get, and scp's typical chmod-after-upload will either silently corrupt the user's expectations or fail the transfer depending on how it is answered. Vita's sceIo has no uid/gid model, no symlink support, and limited attribute control, and libssh's handlers also want to map numeric ids to user and group names, which this platform has no source of. The override mechanism is libssh's own callback surface for SFTP operations — the point is to override only the misbehaving handlers and leave the rest of the protocol to the library.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 chmod, chown and utimes answer with an honest status instead of a false success, and the choice for each is recorded with its reason
- [ ] #2 uid and gid are reported consistently for every file so clients stop trying to resolve names that do not exist
- [ ] #3 no handler reports a symlink for any path
- [ ] #4 clients that probe before acting (scp, sftp batch mode) complete their transfers rather than aborting on the unsupported calls
- [ ] #5 removing and renaming across devices behaves honestly: refused with a clear status, not silently copied
- [x] #6 which opcodes are overridden, and why, is documented in the code and in AGENTS.md
<!-- AC:END -->

## Implementation Plan

<!-- SECTION:PLAN:BEGIN -->
Research findings (libssh 0.12.2 src/sftpserver.c, newlib in $VITASDK):
- The task text assumes a libssh callback surface for overriding handlers. There is none for the default server: message_handlers[] / extended_handlers[] are static, process_client_message() and sftp_get_client_message_from_packet() are static. Only the --wrap layer from TASK-002.01 and pre-filtering whole packets in sh_sftp_data() can change behaviour without forking.
- Stock behaviour today: setstat calls truncate/chmod/chown/utimes with the raw POSIX path (truncate and chown are not wrapped, so they hit sceIo with "/ux0/..." and fail as ENOENT); rename (SSH_FXP_RENAME) and fstat/fsetstat are process_unsupported; the "posix-rename@openssh.com" extended op is advertised in the version reply but has no handler, so OpenSSH sftp's rename always gets OP_UNSUPPORTED; statvfs@openssh.com is unwrapped.
- stat_to_filexfer_attrib() copies st_uid/st_gid/st_mode straight from stat(); the readdir long name prints uid/gid with %d. Vita newlib fills these from sceIoStat, so they are not guaranteed consistent and mode could carry non-file/dir type bits.
- newlib utimes is real (sceIoChstat times) and already used by the touch builtin; chmod/chown/statvfs exist in libc but have no Vita meaning; symlink/readlink are ENOSYS shims in vita_shims.c.

Plan:
1. stat_common(): normalise every result: uid=gid=0 (AC2), st_mode = S_IFDIR|0755 or S_IFREG|0644 (0555/0444 on read-only devices), never S_IFLNK; lstat == stat (AC3). Covers readdir entries since stock stats each name through the wrapped lstat.
2. Wrap chmod/chown/truncate/statvfs additionally (translate path, writable check). chmod and chown: return ENOSYS => SSH_FX_OP_UNSUPPORTED (Vita has no permission or ownership model; pretending success is the failure mode the task warns about). utimes: honour it for real via the translated path (sceIoChstat supports times). truncate: translate, then stat back and fail EIO if the size did not change. statvfs: ENOSYS.
3. rename (AC5): intercept SSH_FXP_RENAME and posix-rename@openssh.com in sh_sftp_data() before the stock callback; parse id/old/new from the reassembled packet; reply through public sftp_reply_status() using a stack sftp_client_message (only sftp+id are read). Resolve both; refuse ROOT/device-root/read-only targets; refuse different devices with SSH_FX_FAILURE "cross-device rename not supported" (not OP_UNSUPPORTED, which makes clients retry another way); plain RENAME onto an existing target fails (SFTP v3 semantics), posix-rename replaces a file by unlink+rename (sceIoRename will not replace) and refuses to replace a directory.
4. Document each overridden opcode and its reason in a block comment in sftp_server.c and in AGENTS.md (AC6); update the README 'sftp refused' lines only if they are now false (TASK-004 owns README scope, so leave unless stale).
5. Verification: no test harness exists and nothing runs on the host. task build of both VPKs is the check; AC4 and device behaviour belong to TASK-002.03. Will say so plainly.

Open questions for Lance before implementing: (a) chmod/chown => OP_UNSUPPORTED vs a quiet success; (b) interception in sh_sftp_data() instead of a libssh callback; (c) AC5 'removing ... across devices' read as rename only, since unlink has no cross-device case.
<!-- SECTION:PLAN:END -->

## Implementation Notes

<!-- SECTION:NOTES:BEGIN -->
Lance approved intercept + wrap (no libssh fork) and OP_UNSUPPORTED for chmod/chown.

Implemented in src/sftp_server.c: stat/lstat normalised (uid/gid 0, nlink 1, plain file/dir mode, never a link); __wrap_chmod/chown/statvfs answer ENOSYS; __wrap_truncate translates and reads the size back; utimes stays honoured; handle_rename() answers RENAME and posix-rename@openssh.com in sh_sftp_data() before the stock callback (cross-device => SSH_FX_FAILURE, device root/read-only refused, plain RENAME onto existing fails, posix-rename unlinks a file target first). CMakeLists.txt gained --wrap=chown,truncate,statvfs. AGENTS.md documents every override.

Evidence: task build produces both VPKs with no warnings; objdump of build/ssh_server_vita shows process_setstat calling __wrap_truncate/__wrap_chmod/__wrap_chown and process_extended_statvfs calling __wrap_statvfs.

Not verified: nothing runs on the host and there is no test harness, so no TDD test was written and AC1-AC5 behaviour is unconfirmed on a device (TASK-002.03). Only AC6 (documentation) is checkable here.

Known limits: setstat applies size, chmod, chown, utimes in order and stops at the first error, so a setstat carrying permissions plus times fails before the times land (scp -p style); fstat/fsetstat stay stock OP_UNSUPPORTED. AC5 'removing ... across devices' was read as rename only. TASK-002.01 is still In Progress, so this sits on unverified groundwork.
<!-- SECTION:NOTES:END -->
