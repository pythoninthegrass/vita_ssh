---
id: TASK-037
title: Queue completed lines instead of overwriting the one pending line
status: Done
assignee:
  - Claude
created_date: '2026-11-18T09:16:00Z'
updated_date: '2026-11-18T18:02:00Z'
labels:
  - shell
dependencies: []
documentation:
  - AGENTS.md
  - src/sshd_session.c
priority: medium
type: bug
ordinal: 70000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
The line editor holds one completed line at a time: src/sshd_session.c copies a finished line into `pending` and sets `pending_ready`, and `sh_session_pump()` runs it on the next poll-loop pass. If a second line completes before the pump consumes the first, it overwrites it and the first command never runs, with no message. The only guard is for the LF of a CRLF pair (a bare LF while a command is queued is swallowed).

Seen on a device on 2026-11-18 while testing TASK-016.06: an interactive `ssh -tt` session fed `cd /ux0/music` and `printenv PWD` in one write echoed both lines, then printed the home directory, so the `cd` was lost. The same thing happens to any pasted multi-line block, or a script piped into a pty session, and also when the pump is busy: a long command (`du`, a large `cat`) can be running while the user types ahead, and every line but the last typed during it is dropped.

Wanted: a completed line is never discarded. Decide in the plan, not assume: a small bounded queue of lines (how many, and what happens when it is full: refuse with a visible message rather than overwrite, since dropping silently is the failure being fixed), whether typed-ahead lines are echoed when they are typed or when they run, and how history, `!n` expansion and the prompt behave for a queued line. The exec path shares `pending` (`on_exec_request()`) and the sftp subsystem checks `pending_ready`, so both need to keep working. Memory is a constraint: SSHD_LINE_MAX bytes per queued line per session, with up to SSHD_MAX_SESSIONS sessions.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [x] #1 Two lines delivered in one chunk to a pty session both run, in order, and the second sees the first's effect (cd then printenv PWD)
- [x] #2 Lines typed while a long command is running run after it finishes, in order
- [x] #3 When the queue is full the editor refuses the line with a visible message and does not overwrite an earlier one
- [x] #4 The CRLF case still works: a line ending \r\n runs once, and a scripted pty session loses no output lines
- [x] #5 An exec session and an sftp session behave as before
- [x] #6 The queue is bounded, and the per-session memory cost is stated in the task notes
<!-- AC:END -->

## Definition of Done
<!-- DOD:BEGIN -->
- [x] #1 task build produces both VPKs
- [x] #2 Verified on a device with a pty session fed two lines in one write
- [x] #3 arm-vita-eabi-size build/ssh_server_vita measured before and after against the size ceiling in AGENTS.md
<!-- DOD:END -->

## Implementation Plan

<!-- SECTION:PLAN:BEGIN -->
Research: finish_line() copies the line into one `pending` slot and sets `pending_ready`; sh_session_pump() consumes it. Typed-ahead bytes are read by sh_server_poll_slice() while a command runs, so every line but the last is overwritten. The pump also calls clear_line() after consuming, which would wipe a half-typed line once typeahead is supported. Baseline: `arm-vita-eabi-size build/ssh_server_vita` text 2304652 (taken before any change).

Decisions:
1. Bounded FIFO of completed lines, SSHD_LINEQ_MAX = 8 slots. Each slot is a heap copy of the line (strdup-style, exact length), not a 4096-byte array, so an idle session costs 8 pointers plus a count (about 40 bytes) and the worst case is 8 x SSHD_LINE_MAX = 32 KB per session (128 KB across 4 sessions), reached only if every queued line is near the limit.
2. Full queue (or allocation failure): the line is refused with a visible `sh: input queue full, line discarded` on stderr, the earlier lines are untouched, and nothing is added to history.
3. Echo at typing time (the editor already echoes as bytes arrive, like tty typeahead); the prompt is written after each command finishes, before the next queued line runs.
4. History and `!n` expansion happen when the line is completed, against history as it stands then. A queued `!!` therefore sees earlier queued lines as history entries, as in bash.
5. CRLF: replace the `pending_ready` guard with a `last_was_cr` flag: an LF right after a CR (and nothing between) is swallowed; any other empty Enter is queued as an empty line. The old guard would have swallowed real empty lines whenever the queue was non-empty.
6. Remove clear_line() from the pump so a half-typed line survives a command.
7. Exec pushes its one command into the same queue; the sftp subsystem check becomes `queue empty`.
8. Pure queue logic goes in src/sh_lineq.c / include/sh_lineq.h (no I/O, no Vita APIs) so it is host tested under ASan/UBSan like the other sh_* modules: TDD with scripts/test_sh_lineq.c + .py, including every push/pop sequence up to a bounded length against a model.

Steps: (a) failing host test, (b) sh_lineq.c, (c) wire into sshd_session.c and the server add_executable list, (d) task build, size after, (e) docs: AGENTS.md test list and server platform rules, (f) device check (pty session fed two lines in one write) is deferred: the user asked on 2026-11-18 for nothing to be pushed to the Vita while another app is under test.

2026-11-18 revision after the device run: (9) EOF on a shell session with lines queued sets `eof_seen` and the session finishes after the last queued line runs; (10) before a shell session ends on `exit` or drained EOF the pump waits for libssh's output buffer to empty (wait_for_room(s, 0)). The device check in step (f) was done once the user lifted the hold; results are in the implementation notes.
<!-- SECTION:PLAN:END -->

## Implementation Notes

<!-- SECTION:NOTES:BEGIN -->
2026-11-18 host work done, device check pending. Added src/sh_lineq.c + include/sh_lineq.h (bounded FIFO, SH_LINEQ_MAX 8) with scripts/test_sh_lineq.{c,py} (written first and seen failing on the missing module, now passing under ASan/UBSan, including all 4096 push/pop sequences of 12 steps against a model). Wired into src/sshd_session.c: finish_line() pushes (refuses with `sh: input queue full, line discarded`, no history entry), exec pushes, sftp accepted only while the queue is empty, the pump pops one line per pass and no longer calls clear_line(), the CRLF guard is now an `after_cr` flag, and the pump redraws a half-typed line after the prompt. CMakeLists.txt and AGENTS.md updated.

Memory per session: sh_lineq_t is 8 pointers + head + len = 40 bytes on ARM32 (idle); each queued line is a heap copy of its exact length, so the bound is 8 x SSHD_LINE_MAX = 32 KB per session, 128 KB across SSHD_MAX_SESSIONS (4), reached only if every slot holds a near-maximal line. This replaces the old always-resident `pending[SSHD_LINE_MAX]` (4 KB per session), so an idle session is 4 KB smaller.

Size: `arm-vita-eabi-size build/ssh_server_vita` text 2304652 before, 2304952 after (+300 bytes); the link succeeded. `task build` produced both VPKs.

Not done: AC #1-#5 and DoD #2 need a device run (pty session fed `cd /ux0/music` and `printenv PWD` in one write; typeahead during a `du`; nine lines to fill the queue; a `\r\n` script; an exec and an sftp session). The user asked not to push anything via sshd while another app is under test, so nothing has been installed or run on the Vita.

2026-11-18 device run (home Vita, installed with `vpk install` over ssh at the user's direction; CLAUDE.local.md updated to drop the prohibition). Build under test: text 2305080 (+428 over the 2304652 baseline).

Two changes beyond the first plan, both found on the device: (1) a piped script closes its stdin right behind the last line, and the server finished a shell session on EOF, dropping any lines still queued; EOF now sets `eof_seen` when lines are queued and the session finishes once they have run. (2) `exit` can now follow other commands within a few milliseconds, and the session was freed with their output still in libssh's buffer (output cut mid-line); the pump now calls wait_for_room(s, 0) before ending the session on exit or drained EOF, as the exec path already did.

Results on the device (ssh -tt, raw transcripts kept): AC1 `cd /ux0/music` + `printenv PWD` in one write: both run, printenv prints /ux0/music (CR separators and LF-only separators). AC2 `du /ux0/app` (about 12 s) with 10 lines typed after it started: Q1..Q8 ran in order after du. AC3 same run: Q9 and Q10 each answered `sh: input queue full, line discarded`, Q1..Q8 untouched. AC4 `echo A\r\necho B\r\n\r\necho C\r\nexit\r\n`: A, B, one empty prompt, C, each once. Piped input with no hold (`printf ... | ssh -tt`): all lines run before the session ends. AC5 exec: `scripts/smoke.sh exec` 118 pass, 0 fail; sftp is covered by the comparison below. Queue capacity is 8 including the line a burst delivers before the pump pops its first line: a client that sends `du` and 8 more lines in one burst has the 8th refused (a test-timing artifact I hit first: ssh does not read stdin until login finishes).

Pre-existing, not caused by this change (compared against the committed HEAD build installed on the same device with the same log): (a) `scripts/smoke.sh pty` fails 12 of 16 once sshd.log is large (about 110 KB and up): its first step is `cat sshd.log`, and the harness types the next lines while that cat is still streaming. HEAD passes 16/16 with a 92 KB log and fails the same 12 with a 236 KB log. (b) A long `cat` in a pty session with a line typed while it streams wedges the session at roughly 65-125 KB of output until `stopped reading for 30 s; dropping the session`; HEAD does the same with a 1.4 MB file. A likely cause is the echo of typed bytes writing from inside the data callback while in_pump is set, so wait_for_room() runs a nested poll slice, which AGENTS.md says is not legal from a libssh callback; not confirmed. (c) 12 MB sftp/scp transfers and the 12 MB exec cat fail on HEAD too. Not fixed here; a follow-up task needs the user's go-ahead.

2026-11-18 AC5 closed on the office network (Vita at its office address, server restarted on the device first because it had stopped answering banners after the network switch): `scripts/smoke.sh sftp` on this build, 83 pass, 0 fail, 1 skip, including the 12 MB sftp and scp transfers and the tree transfers; `scripts/smoke.sh exec` was 118 pass, 0 fail earlier. Correction to the earlier note (c): the 12 MB transfer failures seen at home (on this build and on HEAD) were not reproducible here, so they look like home Wi-Fi flakiness rather than a server defect; TASK-037.01 treats them as in scope only if its root cause turns out to be shared.
<!-- SECTION:NOTES:END -->

## Final Summary

<!-- SECTION:FINAL_SUMMARY:BEGIN -->
Completed lines are now queued instead of overwriting the single pending line. A bounded per-session FIFO (`sh_lineq_t`, 8 slots, in `src/sh_lineq.c` and `include/sh_lineq.h`) holds heap copies of finished lines; `finish_line()` and the exec request push, and `sh_session_pump()` pops one line per poll-loop pass. A full queue refuses the new line with `sh: input queue full, line discarded` and leaves earlier lines and history untouched. The CRLF guard is now an `after_cr` flag, so only an LF directly after a CR is swallowed and other empty Enters are real lines. The pump no longer clears the line buffer, and redraws a half-typed line after the prompt.

Two defects found on the device were fixed in the same change: a client EOF with lines queued (a piped script) now lets the queue drain before the session ends (`eof_seen`), and a session ending on `exit` or a drained EOF waits for libssh's output buffer to empty first, because queued commands run back to back and the session was freed with output unsent.

Verification: `python3 -I scripts/test_sh_lineq.py` passes under ASan/UBSan (order, refusal when full, all 4096 push/pop sequences of 12 steps against a model; written first and seen failing). `task build` produces both VPKs; server text 2304652 before, 2305080 after (+428). On the device: two lines in one write run in order with `cd` visible to `printenv PWD`; 10 lines typed during a 12 s `du` ran Q1..Q8 in order and refused Q9 and Q10 with the message; a CRLF script ran each line once; piped input with no hold ran every line; `scripts/smoke.sh exec` 118 pass 0 fail and `scripts/smoke.sh sftp` 83 pass 0 fail 1 skip. Memory: 40 bytes idle per session (replacing the old 4 KB `pending` buffer), at most 8 x 4096 = 32 KB per session if every slot holds a maximal line.

Risks and follow-ups: typing while a long command streams a large output (for example `cat` of 1.4 MB) wedges a pty session until the server drops it after 30 s; HEAD before this change does the same, and it makes `scripts/smoke.sh pty` fail once `sshd.log` is large (12 of 16 at 236 KB, 16 of 16 at 92 KB). It is tracked as TASK-037.01 with the cause still unconfirmed.
<!-- SECTION:FINAL_SUMMARY:END -->
