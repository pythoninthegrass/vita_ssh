---
id: TASK-028
title: Let long-running builtins yield to the poll loop so other sessions can connect
status: Done
assignee:
  - Claude
created_date: '2026-11-17T17:45:00Z'
updated_date: '2026-11-17T23:30:00Z'
labels:
  - shell
  - sshd
dependencies: []
references:
  - src/sshd_builtins.c
  - src/sshd_session.c
  - src/sshd.c
  - AGENTS.md
  - docs/vcmd.md
modified_files:
  - src/sshd_session.c
  - src/sshd_builtins.c
  - include/sshd.h
  - AGENTS.md
priority: medium
type: bug
ordinal: 59000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
Found while verifying TASK-015.02 on a device (the work Vita, 2026-11-17): while `du /ux0` runs (about 170 s there), a second ssh session cannot even finish the SSH banner exchange (ConnectTimeout 10 s expired). The same happens with `du /ux0 | wc -l`, so pipes did not cause it. A builtin that is CPU or I/O bound and never calls `sh_server_poll_slice()` stops the single poll-loop thread, and with it every other session, the accept path and the Vita console.

AGENTS.md already states the rule for output (`wait_for_room()` runs `sh_server_poll_slice()` so every session's socket is still read) and `vcmd` does it for the network (`net_wait()`), but the tree walkers do not: `du`, `cp -r`, `rm -r`, and `grep` over many files are the candidates, and `du` is the one measured. Only `du` has a measurement; the others are suspects until timed.

Limits to settle in the open, not paper over: `sh_server_poll_slice()` is one `ssh_event_dopoll` and is not reentrant from inside a libssh callback, so it is legal only from the pump (`in_pump`), the same restriction `chan_write_raw()` has. A slice reads sockets and completes handshakes; per AGENTS.md, other sessions' commands and sftp pumps still run only when the running command finishes, so this task makes the server answer and accept during a long command, not run two commands at once. Decide how often a walk yields (every N entries or every N ms) so the cost is small, and whether the 170 s itself is a defect worth a separate look (it may be the sceIo stat per entry on a card with many files).

Do not widen this to Ctrl-C handling; that is its own task and depends on this one.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [x] #1 While `du /ux0` runs on a device, a second ssh connection completes its banner and authenticates within the ConnectTimeout the smoke script uses (5 s), measured and recorded.
- [x] #2 The same holds for the other tree walkers that are timed and found to starve the loop (cp -r, rm -r, grep over a tree), each named in the task notes with its measurement.
- [x] #3 A walk yields only from the pump; a call from a libssh callback never runs a poll slice.
- [x] #4 A walk's total time grows by a stated, measured amount at most, recorded before and after.
- [x] #5 AGENTS.md says which builtins yield and how often, and the rule for a new long-running builtin matches what the code does.
<!-- AC:END -->

## Definition of Done
<!-- DOD:BEGIN -->
- [x] #1 task build produces both VPKs
- [x] #2 Verified on a device over a real ssh session with a second session connecting mid-command
<!-- DOD:END -->

## Implementation Plan

<!-- SECTION:PLAN:BEGIN -->
1. Add `sh_session_yield(s)` to sshd_session.c (declared in include/sshd.h): when `in_pump` is set and at least YIELD_INTERVAL_MS (100 ms, sceKernelGetSystemTimeWide) passed since the last yield, run `sh_server_poll_slice(0)`; otherwise a no-op, so a call from a libssh callback never polls (AC3).
2. Call it beside the per-entry `sh_session_interrupted()` check in the three tree walkers: `rm -r`, `cp -r`, `du`. `cmd_grep` has no directory walk (no opendir/readdir), so it is not a tree walker here; note that in the task.
3. Per-entry cost is a timer read; a slice is at most once per 100 ms. Measure du before/after on a device (AC4).
4. Update AGENTS.md: which builtins yield, how often, and the rule for a new long-running builtin (AC5).
5. `task build` for both VPKs; device verification (AC1, AC2, DoD2) needs the user's Vita and is done by running `du /ux0` with a second ssh connecting mid-command.

No host test exists for this (it needs the Vita poll loop); compile and device run are the only checks, as AGENTS.md states.

Final: also yields once per 64 KB chunk in copy_file() (cp, cp -r, mv across devices), found on the device when a large file copied unpolled. AGENTS.md says so.
<!-- SECTION:PLAN:END -->

## Implementation Notes

<!-- SECTION:NOTES:BEGIN -->
Implemented `sh_session_yield()` (src/sshd_session.c, include/sshd.h): no-op unless `in_pump`, else one `sh_server_poll_slice(0)` at most every 100 ms (`YIELD_INTERVAL_US`, sceKernelGetSystemTimeWide). Called once per directory entry in `du`, `cp -r` and `rm -r`. `cmd_grep` has no directory walk, so it was not changed.

Device run (work Vita, 2026-11-17, server installed over SSH, result.txt `promote package: 0x00000000`). Baseline before: `du /ux0` 167.5 s, and per the earlier finding a second ssh timed out in the banner. After: `du /ux0` 177 s and 179 s over two runs (about +10 s, +6%, two samples against one baseline). During the second run, three probes at 20, 40 and 60 s into the walk each saw the banner at +1 s and `Authenticated ... publickey` at +2 s (ssh -v, BatchMode, timestamps from `date`). The probes' `pwd` commands ran only after `du` ended, as AGENTS.md says they must.

Not done: `cp -r` and `rm -r` were not timed, so AC2 stays open; they carry the same yield call but nothing measured shows they starve the loop. Not committed.

cp -r and rm -r device run (work Vita, 2026-11-17). Tree: /ux0/data/retroarch (161.6 MB, 85 s to walk with du); copy target /ux0/data/t028-cp, removed afterwards (source intact, scratch gone). Probes are `ssh -v -o BatchMode=yes pwd` killed after 12 s, timestamps from `date`.

rm -r (per-entry yield build): 155 s total; three probes mid-walk each saw banner at +0 s and auth at +1 s. No no-yield baseline was taken for rm -r, so its added cost is not measured; du's is +10 s (+6%) from the earlier note.

cp -r, first run (per-entry yield only): 390 s total; probes 1 and 2 passed (banner +0 s, auth +2 s) and probe 3 got no banner in 12 s. Cause not established. cp's copy_file() had no yield in its read/write loop, so one large file runs unpolled; added `sh_session_yield(s)` per 64 KB chunk in copy_file() (also covers cp of one file and cross-device mv).

cp -r, second run (chunk yield, server reinstalled, result.txt 0x00000000): 393 s total (390 s without the chunk yield, so no measurable cost). Probes 1-3: banner +0 s, auth +1 s, +3 s, +2 s. Probes 4-6 got no banner, and sshd.log shows `refused ...: 4 sessions already open` for each: the cp session plus three abandoned probes held all four SSHD_MAX_SESSIONS slots. That is the slot cap, not starvation, but it exposes a separate point: a probe killed mid-command is not reaped until the running command finishes (log at the end: three `handshake failed ... disconnected by user` lines). Not changed here; a closed client holding a slot until the running command ends is TASK-029's territory (Ctrl-C and noticing the client) or its own task.

sshd.log also prints `poll: loop stalled for 392919 ms` after a long command; that watchdog measures the gap between outer loop passes and does not know the yield polled meanwhile.
<!-- SECTION:NOTES:END -->

## Final Summary

<!-- SECTION:FINAL_SUMMARY:BEGIN -->
Long-running builtins now yield to the poll loop, so a second ssh session can connect while one runs. Added sh_session_yield() (src/sshd_session.c): no-op unless the session is inside sh_session_pump(), otherwise one zero-wait poll slice at most every 100 ms. Called once per directory entry in du, cp -r and rm -r, and once per 64 KB chunk in copy_file() (cp, mv across devices). grep has no directory walk and is unchanged. AGENTS.md documents which builtins yield, how often, and the rule for a new long-running builtin.

Verified on the work Vita (2026-11-17) with the server installed over SSH. du /ux0: 167.5 s before, 177 and 179 s after (about +6%); second-session banner at +1 s and auth at +2 s mid-walk, where before it timed out. rm -r on a 161.6 MB tree: 155 s, probes banner +0 s, auth +1 s. cp -r: 390 s before the chunk yield, 393 s after, probes 1-3 banner +0 s, auth +1 to +3 s. No no-yield baseline was taken for rm -r, so its cost is unmeasured. Compile and device runs are the only checks available for this code.

Follow-up: a client that disconnects mid-command keeps its session slot until the running command ends, so abandoned connections can fill all four slots (sshd.log: refused: 4 sessions already open). Filed separately.
<!-- SECTION:FINAL_SUMMARY:END -->
