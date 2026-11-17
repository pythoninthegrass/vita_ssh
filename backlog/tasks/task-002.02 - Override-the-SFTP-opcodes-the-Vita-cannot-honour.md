---
id: TASK-002.02
title: Override the SFTP opcodes the Vita cannot honour
status: Done
assignee:
  - '@claude'
created_date: '2026-11-15T22:57:00Z'
updated_date: '2026-11-16T08:15:00Z'
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
- [x] #1 chmod, chown and utimes answer with an honest status instead of a false success, and the choice for each is recorded with its reason
- [x] #2 uid and gid are reported consistently for every file so clients stop trying to resolve names that do not exist
- [x] #3 no handler reports a symlink for any path
- [x] #4 clients that probe before acting (scp, sftp batch mode) complete their transfers rather than aborting on the unsupported calls
- [x] #5 removing and renaming across devices behaves honestly: refused with a clear status, not silently copied
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

Open questions for the user before implementing: (a) chmod/chown => OP_UNSUPPORTED vs a quiet success; (b) interception in sh_sftp_data() instead of a libssh callback; (c) AC5 'removing ... across devices' read as rename only, since unlink has no cross-device case.

Final design, where it departed from the plan above: permission-only SETSTAT and FSETSTAT are answered OK when they ask for the mode already reported (decided with the user as option 1), because scp sends one after every upload; anything else, including times, stays unsupported. The overrides sit in two seams only, --wrap of the libc calls the stock handlers make and handle_rename()/handle_perm_setstat() answering packets in sh_sftp_data() before the stock callback, since libssh 0.12.2 exposes no handler callbacks.

AC1 was verified with scripts/sftp_probe.py: chmod and chown are refused (smoke.sh), and a SETSTAT with times either fails or reads back the time that was sent; on the device it succeeded and read back within 2 s, so utime is honoured, not faked.
<!-- SECTION:PLAN:END -->

## Implementation Notes

<!-- SECTION:NOTES:BEGIN -->
The user approved intercept + wrap (no libssh fork) and OP_UNSUPPORTED for chmod/chown.

Implemented in src/sftp_server.c: stat/lstat normalised (uid/gid 0, nlink 1, plain file/dir mode, never a link); __wrap_chmod/chown/statvfs answer ENOSYS; __wrap_truncate translates and reads the size back; utimes stays honoured; handle_rename() answers RENAME and posix-rename@openssh.com in sh_sftp_data() before the stock callback (cross-device => SSH_FX_FAILURE, device root/read-only refused, plain RENAME onto existing fails, posix-rename unlinks a file target first). CMakeLists.txt gained --wrap=chown,truncate,statvfs. AGENTS.md documents every override.

Evidence: task build produces both VPKs with no warnings; objdump of build/ssh_server_vita shows process_setstat calling __wrap_truncate/__wrap_chmod/__wrap_chown and process_extended_statvfs calling __wrap_statvfs.

Not verified: nothing runs on the host and there is no test harness, so no TDD test was written and AC1-AC5 behaviour is unconfirmed on a device (TASK-002.03). Only AC6 (documentation) is checkable here.

Known limits: setstat applies size, chmod, chown, utimes in order and stops at the first error, so a setstat carrying permissions plus times fails before the times land (scp -p style); fstat/fsetstat stay stock OP_UNSUPPORTED. AC5 'removing ... across devices' was read as rename only. TASK-002.01 is still In Progress, so this sits on unverified groundwork.

First on-device run (smoke.sh sftp) found three defects, all outside the 002.02 opcodes but in the SFTP path from 002.01:
1. Crash: libssh's readdir long name calls ctime_r() and passes the result to asctime_r() unchecked; newlib's localtime_r returns NULL for a time sceRtc rejects, and the synthetic device-directory stat had mtime 0. Fixed by usable_time() in src/sftp_server.c.
2. Hang: on_channel_eof/on_channel_close only set s->finished and sh_session_pump() never acted on it for an SFTP session, so the channel stayed open and the client's ssh never exited. Fixed in sh_session_pump().
3. No visibility: added one log line per SFTP request (not READ/WRITE).

Added scripts/smoke.sh sftp half, scripts/parse_dump.py (crash dump symboliser) and docs/development.md. After the fixes ls / works on device; the remaining smoke.sh sftp checks have not been seen to pass yet, so no further AC is checked.

Correction to the note above: 'ls / works on device' is inferred from sshd.log, which shows INIT, REALPATH, LSTAT, OPENDIR, READDIR x2 and CLOSE handled without a failure line; the client's own output was never seen.

Device verification, scripts/smoke.sh sftp against the Vita (server code built Nov 16 2026 01:45:35): 35 pass, 0 fail, 1 informational.
- AC2: ls -ln lists the scratch files and every uid and gid is 0.
- AC3: ls -ln of the scratch dir and of /ux0/data shows no symlink entries.
- AC4: scp -s upload and download exit 0, sftp put -p completes, and a '-' prefixed failing command does not abort the batch. scp -s -p exits 0 but its times cannot be honoured (FSETSTAT with times stays unsupported), reported as informational.
- AC5: cross-device rename is refused with the source kept and no copy left; rename onto an existing directory and onto /os0 are refused; rename onto an existing file replaces it (posix-rename).
- AC1 stays unchecked: chmod and chown are verified refused, but utimes is not exercised by any stock client (sftp and scp set times with FSETSTAT, which stays unsupported), so its honoured path has not run on a device.

Support added along the way: permission-only SETSTAT/FSETSTAT is answered OK when it asks for the mode already reported (decided with the user as option 1), because scp sends one after every upload. Consequence: scp of a file whose mode is not 0644 reports failure by design.

Transport lessons recorded in AGENTS.md: no blocking libssh calls from a callback (reply queue + non-blocking session), --wrap also catches calls made inside libc (opendir), wait for the channel close before dropping the transport, and every stat handed to libssh needs a time localtime_r accepts.
<!-- SECTION:NOTES:END -->

## Final Summary

<!-- SECTION:FINAL_SUMMARY:BEGIN -->
The SFTP opcodes the Vita cannot honour now answer honestly instead of falling through to the stock handlers: stat and lstat report owner 0:0, mode 0644/0755 (0444/0555 on read-only devices) and never a symlink; chmod, chown and statvfs answer operation-unsupported; truncate is translated and verified by reading the size back; utimes is honoured and verified on the device; rename and posix-rename@openssh.com are implemented (cross-device and read-only targets refused with a failure status, never a copy, plain rename onto an existing path fails, posix-rename replaces a file). A permission-only SETSTAT/FSETSTAT for the mode already reported succeeds, which is what scp sends after an upload. Each override and its reason is documented in AGENTS.md and src/sftp_server.c.

Verification on a Vita (scripts/smoke.sh sftp: 46 pass, 0 fail, 1 informational): uid and gid are 0 for every listed entry and no entry is a symlink; chmod and chown are refused; scp -s upload and download, sftp put -p and a batch with a failing '-' command all complete; cross-device rename, rename onto a directory and onto /os0 are refused with the source kept and no copy left; scripts/sftp_probe.py sent a SETSTAT with times and read the same time back.

Risks and follow-ups: scp of a file whose mode is not 0644 transfers the data and then reports an error, by design; scp -p and sftp put -p cannot set times through FSETSTAT, so the data arrives and the times do not (informational line in the smoke test); verified from macOS only.
<!-- SECTION:FINAL_SUMMARY:END -->
