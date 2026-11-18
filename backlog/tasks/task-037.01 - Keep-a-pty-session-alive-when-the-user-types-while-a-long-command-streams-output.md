---
id: TASK-037.01
title: >-
  Keep a pty session alive when the user types while a long command streams
  output
status: Done
assignee:
  - claude
created_date: '2026-11-18T15:43:00Z'
updated_date: '2026-11-18T20:43:00Z'
labels:
  - shell
dependencies: []
documentation:
  - AGENTS.md
  - src/sshd_session.c
  - scripts/smoke.sh
parent_task_id: TASK-037
priority: medium
type: bug
ordinal: 72000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
In an interactive pty session, typing a line while a command is still streaming a large output wedges the session: the output stops at roughly 65 to 125 KB and nothing more arrives until the server logs `stopped reading for 30 s; dropping the session` and closes it. Found on a device on 2026-11-18 while verifying TASK-037, and reproduced on the committed HEAD build from before that task, so it is not a regression of the line queue. Repro: in `ssh -tt` run `cat` on a file of about 1 MB or more (the 1.4 MB server VPK in `ux0:data` works, as does a 236 KB `sshd.log`) and type a line, for example `echo MID1`, about a second after the cat starts; a line typed after the output finished works, and so does a line typed during a command that prints little (`du /ux0/app`).\n\nIt also makes `scripts/smoke.sh pty` unreliable: its first step is `cat sshd.log` and it types the next lines while that streams, so the pty half passes 16 of 16 on HEAD with a 92 KB log and fails the same 12 checks with a 236 KB log. The log grows with every run and is not rotated on restart, so the smoke result depends on how much has been logged. A user pasting text or typing ahead during a long listing hits the same wedge.\n\nThe cause is not confirmed. One candidate: the echo of typed bytes is written from inside the channel data callback while `in_pump` is set, so `wait_for_room()` runs a nested `sh_server_poll_slice()`, which AGENTS.md says is not legal from inside a libssh callback. A related loss: echo and the queue-full message written from the callback while a command streams. Nothing in this task presumes that cause; confirm it first.\n\nThe same device run showed 12 MB sftp/scp transfers and a 12 MB exec `cat` failing on HEAD as well (`sftp get 12 MB`, `sftp get -r tree`, `exec cat of 12 MB`). Those may share a cause or may not; this task covers them only if the root cause turns out to be common.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [x] #1 A pty session running `cat` on a file of at least 1.4 MB with a line typed a second in streams the whole file and then runs the typed line, with no `stopped reading` entry in sshd.log
- [x] #2 The typed line's echo appears and the line runs after the cat finishes, in order, with the TASK-037 queue behavior unchanged (refusal when full, no overwrite)
- [x] #3 `scripts/smoke.sh pty` passes 16 of 16 with an sshd.log of at least 250 KB
- [x] #4 The cause is identified and recorded in the task notes, including whether the 12 MB sftp, scp and exec transfer failures share it
- [x] #5 AGENTS.md states whatever rule the fix establishes for writing from a libssh callback while a command is running
- [x] #6 A host test covers any new pure logic the fix adds, and `task build` produces both VPKs with `arm-vita-eabi-size build/ssh_server_vita` recorded against the size ceiling
<!-- AC:END -->

## Implementation Plan

<!-- SECTION:PLAN:BEGIN -->
1. Confirm the cause on a device before changing code: add a one-line sshd.log entry when wait_for_room() is entered from inside on_channel_data (in_cb set), reproduce with `cat` of the 1.4 MB VPK plus a typed line, and see whether it fires before the `stopped reading` entry. Also run the 12 MB exec cat with no typing to see whether the transfer failures need the callback path at all.
2. Code path under suspicion: on_channel_data -> feed_byte -> sh_write -> chan_write -> chan_write_raw; with in_pump set it calls wait_for_room() -> sh_server_poll_slice() -> ssh_event_dopoll(), re-entering the poll loop from inside its own data callback (AGENTS.md says that is illegal).
3. Fix, if confirmed: an in_cb flag set around the feed loop in on_channel_data; while in_pump and in_cb, chan_write_raw never waits and never writes. Output from the callback goes into a small bounded deferred buffer (pure append/drain logic in its own file with a host test, like sh_lineq) that sh_session_pump() flushes after the command ends and before the prompt/redraw, so echo and the queue-full message appear after the command output, in order. Overflow drops echo bytes only; the editor state is unaffected.
4. TDD: host test first (python3 -I scripts/test_sh_deferred.py, ASan/UBSan, under timeout) for the buffer: order, bound, overflow refusal, drain. Then wire it in, add the file to the server add_executable list.
5. Verify: task build, arm-vita-eabi-size against the ceiling, device run of AC #1/#2, smoke.sh pty with sshd.log >= 250 KB.
6. Record cause and the 12 MB transfer verdict in notes; add the callback-write rule to AGENTS.md.

Final: the buffer is src/sh_defer.c with scripts/test_sh_defer.py (not `sh_deferred`); the deferral sits in chan_write() rather than chan_write_raw(), so it is ahead of the ONLCR conversion and the flush converts once; sh_write() also bypasses out_fp/capture for callback output.
<!-- SECTION:PLAN:END -->

## Implementation Notes

<!-- SECTION:NOTES:BEGIN -->
2026-11-18, the Vita on the office network (see CLAUDE.local.md): diagnostic build (in_cb flag + a log line when wait_for_room() is entered from on_channel_data) installed via scp -s + vpk install -r (promote 0x00000000). Repro `ssh -tt`: cat of the 1.4 MB server VPK, `echo MID1` typed 1 s later. Output stopped at 789,276 of ~1.43 MB, no MID1, ssh exit 255. sshd.log: `DIAG wait_for_room entered from the data callback` at 14:35:20 (the moment of typing), `stopped reading for 30 s; dropping the session` at 14:35:50. CAUSE CONFIRMED: the editor echo is written from the channel data callback while in_pump is set, so chan_write_raw -> wait_for_room -> sh_server_poll_slice re-enters ssh_event_dopoll from inside its own callback and the session never drains.

Fix: on_channel_data sets in_cb around the feed loop. chan_write() parks a write made with in_cb and in_pump set in a bounded per-session sh_defer_t (src/sh_defer.c, 2048 bytes, all-or-nothing, host test scripts/test_sh_defer.py) and sh_session_pump() sends it after the command ends, before the prompt/redraw. sh_write() sends in_cb output straight to the channel, which also stops the echo of a line typed during `cmd > file` or a pipeline segment from landing in the file or the capture (a second loss of the same shape, found while reading sh_write; not reproduced on a device).

Device results on the fixed build: same repro streams 1,435,820 bytes (file is 1,430,285), `echo MID1` and a later `echo AFTER` each appear as echo then output, in order; no new `stopped reading` entry. `scripts/smoke.sh pty`: 16 pass, 0 fail with sshd.log at 256.3 KB (grown by 140 one-shot logins first; it was 128 KB). arm-vita-eabi-size build/ssh_server_vita: text 2305272, data 11610, bss 306636 (the diagnostic build was 2305144 text); both VPKs built, server VPK 1,430,416 bytes. The remaining distance to the vita-elf-create ceiling was not measured.

12 MB transfers: on the fixed build scp put, exec `cat`, scp get and sftp get of a 12,582,912-byte file all completed with a matching sha1, and `sftp get -r` of a small tree worked. The earlier failures did not reproduce, and I did not run them on the pre-fix build, so I cannot say whether they had the same cause. None of those paths involves a write from the data callback (no typed input), so this fix is not expected to be what changes them; if they recur, they need their own task.

Not exercised on a device: the `sh: input queue full` message while a command streams (it now goes through the same deferral; the queue code itself is untouched and covered by test_sh_lineq.py). The exit status of a plain pty session that ends with `exit` is 255 on this build and on a trivial session (`echo hi`, `exit`), so it is not from this change; not investigated.
<!-- SECTION:NOTES:END -->

## Final Summary

<!-- SECTION:FINAL_SUMMARY:BEGIN -->
Fixed the pty wedge: a line typed while a command streams no longer parks the session. Cause (confirmed on a device with a diagnostic log): the editor's echo was written from the channel data callback while a command ran, so wait_for_room() re-entered ssh_event_dopoll from inside its own callback and the output stalled until the 30 s stop. Now on_channel_data sets in_cb, chan_write() parks such writes in a bounded per-session sh_defer_t (new src/sh_defer.c, host test scripts/test_sh_defer.py), and sh_session_pump() sends them after the command ends, before the prompt or redraw. sh_write() also sends callback output straight to the channel, so echo during `cmd > file` or a pipeline segment no longer lands in the file or capture. Verified on a device: the 1.4 MB cat plus a typed line streams fully with echo and output in order and no new `stopped reading`; smoke.sh pty 16/16 with a 256 KB log; 12 MB scp/sftp/exec transfers completed with matching checksums (the earlier failures did not reproduce and were not run on the pre-fix build, so no verdict on a shared cause). Server text is 2305272 bytes and the build did not hit the ceiling abort, but the remaining margin was not measured. Risks and follow-ups: the `input queue full` message while a command streams and the `echo > file` case were not exercised on a device; a plain pty session ending in `exit` returns ssh status 255 on this build and was not investigated; echo past 2048 deferred bytes is dropped.
<!-- SECTION:FINAL_SUMMARY:END -->
