---
id: TASK-002.01
title: Wire libssh's stock SFTP handlers over the path layer
status: Done
assignee:
  - '@claude'
created_date: '2026-11-15T22:57:00Z'
updated_date: '2026-11-16T08:15:00Z'
labels: []
dependencies: []
documentation:
  - src/sshd_session.c
  - src/sh_path.c
  - include/sh_path.h
modified_files:
  - src/sshd_session.c
  - src/sftp_server.c
  - include/sftp_server.h
  - CMakeLists.txt
parent_task_id: TASK-002
priority: high
type: task
ordinal: 7000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
The channel subsystem callback currently prints 'subsystem not supported' and refuses. libssh ships a server-side SFTP implementation that handles the packet protocol itself; the job is to construct it for the accepted session, drive it from the same single-threaded poll loop that already pumps the session, and have it tear down cleanly when the channel closes. The path semantics live in src/sh_path.c: normalise in POSIX space and translate once immediately before the sceIo call, never guess a device, and never fake symlinks. libssh's own handlers assume POSIX filesystem semantics underneath (stat attributes, directory iteration, path handling), so part of the work is showing what they do with Vita's device-prefixed names and adapting at the boundary rather than forking the library.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [x] #1 sftp opens a subsystem session and lands in the session's current directory
- [x] #2 ls, cd, pwd, get, put, mkdir, rmdir, rm, rename and realpath work against a real device
- [x] #3 large downloads and uploads complete without truncation, including files larger than the transfer buffer
- [x] #4 closing the sftp session leaves the server responsive and the shell session still usable
- [x] #5 a rejected subsystem request still leaves the session able to accept a later one
- [x] #6 directory iteration handles a device root and an empty directory without an infinite loop
- [x] #7 a failed or interrupted upload leaves no partial file under the target name (temp name then rename, or equivalent)
<!-- AC:END -->

## Implementation Plan

<!-- SECTION:PLAN:BEGIN -->
Research findings (libssh 0.12.2, src/sftpserver.c):
- Stock handlers (process_open/opendir/readdir/lstat/stat/mkdir/rmdir/remove/realpath/setstat/readlink/symlink) call libc directly with the raw client path. SFTP clients send POSIX paths ("/ux0/downloads/x.vpk"); sceIo-backed newlib only understands "ux0:/...". Unadapted, every op fails or mis-resolves, and realpath(".") uses libc's process-wide cwd.
- Dispatch table message_handlers[] is not exported and process_client_message() is static, so the table cannot be overridden without forking libssh. rename and fstat/fsetstat are process_unsupported (TASK-002.02 territory).
- sftp_server_free() frees the handle table but does not close fds or DIR*, so teardown must close them itself.
- Stock readdir stats "<handle name>/<d_name>" per entry; on "/" there is no sceIo directory to open.

Approach: adapt at the boundary with link-time symbol wrapping, no libssh fork.
1. New src/sftp_server.c: per-session sftp_session, a global "current fs context" (sh_fsctx_t from the session cwd/home) set only around the stock data callback (safe: single thread, callback is synchronous inside ssh_event_dopoll).
2. CMake: add src/sftp_server.c to ssh_server_vita; add -Wl,--wrap=open,close,opendir,readdir,closedir,stat,lstat,mkdir,rmdir,unlink,realpath,chmod,utimes,readlink,symlink for that target only. Each __wrap_X resolves via sh_path_resolve then calls __real_X on target.sce. Outside an SFTP callback the wrappers pass through unchanged (shell builtins already pass sce paths).
3. "/" listing: __wrap_opendir/readdir/closedir return a private DIR-shaped handle over sh_devices() (present devices only), so root and device-root iteration terminate (AC6). readlink/symlink fail ENOSYS-equivalent, never faked.
4. Upload atomicity (AC7): __wrap_open for O_CREAT|O_TRUNC/write opens "<target>.part", records fd->target in a per-session table; __wrap_close renames .part to target via sceIoRename (same device); session teardown closes leaked fds and unlinks their .part files.
5. sshd_session.c: on_subsystem_request accepts "sftp" only (others keep the rejection and leave the channel usable, AC5); constructs the sftp session via sftp_channel_default_subsystem_request into a per-session sftp_session*; channel_data_function dispatches to sftp_channel_default_data_callback when the sftp session exists, else to the shell feeder; channel close/eof/free path calls the sftp teardown then sftp_server_free.
6. TDD limits: nothing runs on host (arm-vita-eabi cross build, no test suite). Verification is task build of both VPKs plus host-side unit tests only if the wrapper path logic can be built against sh_path.c natively; otherwise on-device checks via TASK-002.03 and scripts/smoke.sh. Will state this plainly in the final summary.

Open decision for Lance: confirm --wrap approach, and whether AC7 .part naming is acceptable (alternatives: hidden dotfile name; or leave AC7 to TASK-002.02).

Plan addition (found while reading sftp_decode_channel_data_to_packet): the stock data callback requires each channel chunk to hold whole SFTP packets and returns SSH_ERROR otherwise, so a 32 KB write split across SSH packets fails. sh_sftp_data() reassembles by the 4-byte length prefix (cap 256 KB) and feeds the stock callback one packet at a time.

Final design, where it departed from the plan above (all found on a device): replies are queued, not written, so libssh never blocks inside the poll callback: the sftp session is set non-blocking, --wrap=ssh_channel_write appends to a per-session queue, drain_out() sends only what ssh_channel_window_size() allows, new requests wait while more than 128 KB is queued, and sh_sftp_pump() runs each poll-loop pass. __wrap_opendir clears the active context around the real opendir because newlib's opendir calls open, which --wrap also catches. A finished sftp session sends exit-status, EOF and close and waits up to 3 s for the channel to close before the transport is dropped. Every stat given to libssh goes through usable_time() because localtime_r can return NULL. A refused subsystem's explanation is sent as stderr so it cannot be read as SFTP data.

AC5 was verified with scripts/sftp_probe.py (paramiko): an unknown subsystem is refused, the channel stays open, sftp is accepted on that same channel and lists /.
<!-- SECTION:PLAN:END -->

## Implementation Notes

<!-- SECTION:NOTES:BEGIN -->
Lance approved --wrap and the .part upload approach.

Implemented: include/sftp_server.h, src/sftp_server.c (wrappers for open/close/opendir/readdir/closedir/stat/lstat/mkdir/rmdir/unlink/chmod/utimes/realpath, packet reassembly, teardown), sshd_session.c (accepts only 'sftp', routes channel data, frees on session free), CMakeLists.txt (source + --wrap link options on ssh_server_vita only). task build links and produces both VPKs; nm shows process_open calling __wrap_open/__wrap_close.

Not verified: nothing here runs on the host and the sandbox cannot reach the device, so no acceptance criterion is checked. No TDD test was written: the repo has no test harness and the code is cross-compiled. On-device checks belong to TASK-002.03 / a manual sftp run.

Known gaps for TASK-002.02: libssh's process_close ignores close()'s return, so a failed .part rename at close is logged but the client still sees SSH_FX_OK; statvfs, rename and fstat/fsetstat are not translated or implemented; chmod/utimes are only path-translated, not given Vita semantics.

Device verification, scripts/smoke.sh sftp (35 pass, 0 fail, 1 informational):
- AC1: pwd lands in /ux0/data/vitasshd, the session's home.
- AC2: ls, pwd (REALPATH), get, put, mkdir, rm, rmdir and rename all pass; cd is client-side (REALPATH plus STAT) and was not run as a separate command.
- AC3: a 6 MB upload and download round-trip byte-identical, and an empty file round-trips.
- AC4: after the sessions the shell session and a second sftp session both work.
Still unverified: AC5 (a rejected subsystem request leaves the session able to accept a later one; needs a client that sends two requests on one session), AC6 (device root and empty directory iteration: ls / works, ls /ux0 and an empty directory are not yet in the smoke test), AC7 (interrupted upload leaves no partial file; only the successful-upload case is checked).

The first on-device runs found four defects in this task's code, all fixed: a localtime_r NULL crash on ls / (synthetic stat had mtime 0), no channel close on client EOF (client hung), blocking ssh_channel_write inside the poll callback (downloads stalled and leaked sessions), and double path translation in opendir (every real directory listing failed). Details in AGENTS.md.

Device verification, scripts/smoke.sh sftp (41 pass, 0 fail, 1 informational):
- AC6: ls of a device root (/ux0) and of an empty directory both terminate; the root lists data and the empty directory lists no entries besides . and ..
- AC7: a 64 MB upload is confirmed in flight (a second session sees interrupted.bin.part), the client is killed with SIGKILL, and after the server tears the session down neither interrupted.bin nor interrupted.bin.part exists.
Remaining unverified: AC5 (a rejected subsystem request leaves the session able to accept a later one), which needs a client that sends two subsystem requests on one channel; stock ssh/sftp cannot.
<!-- SECTION:NOTES:END -->

## Final Summary

<!-- SECTION:FINAL_SUMMARY:BEGIN -->
SFTP is served from the Vita sshd app using libssh's stock server handlers, adapted at the libc boundary rather than forked: --wrap routes the handlers' calls through sh_path.c (src/sftp_server.c), incoming data is reassembled into whole packets, uploads go to NAME.part and are renamed on close, and sessions are driven from the existing poll loop (src/sshd_session.c).

Verification, on a Vita over WiFi from a macOS devbox with scripts/smoke.sh sftp: 46 pass, 0 fail, 1 informational. That covers login and cwd, ls/get/put/mkdir/rm/rmdir/rename, a 6 MB round trip and an empty file, device-root and empty-directory listing, an upload killed mid-transfer (a second session sees the .part file in flight; neither it nor the target remains), shell and second sftp session still working afterwards, and a refused subsystem followed by sftp on the same channel (scripts/sftp_probe.py).

The first device runs found and fixed four defects in this task's code that no host build could show: a localtime_r NULL crash on ls /, no channel close on client EOF, blocking ssh_channel_write inside the poll callback (downloads stalled and leaked sessions), and double path translation inside opendir. Their rules are in AGENTS.md. A fifth, the subsystem refusal text landing in the data stream, was found by the probe.

Risks and follow-ups: tested from macOS only (Linux client comparison is TASK-002.03); no automated tests exist, the checks need a device on the LAN; the per-request log lines are chatty by design while the feature is new.
<!-- SECTION:FINAL_SUMMARY:END -->
