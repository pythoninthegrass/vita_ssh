---
id: TASK-032
title: Expand $? to the exit status of the last command
status: Done
assignee:
  - claude
created_date: '2026-11-17T20:44:00Z'
updated_date: '2026-11-19T07:07:00Z'
labels:
  - shell
dependencies:
  - TASK-020
references:
  - src/sshd_builtins.c
  - src/sshd_session.c
documentation:
  - AGENTS.md
priority: medium
type: enhancement
ordinal: 63000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
The shell reports a command's exit status to an exec client but gives a person or a one-liner no way to look at it: there is no `$?`. After `grep foo f` or `cp a b` there is nothing to ask what happened except to chain on it with `&&`/`||` (TASK-025), which only branches and cannot print or compare the number. `$?` is the most used special parameter in sh, and `echo $?` is the first thing anyone types to check a command.

Needs a per-session record of the last command's status, which TASK-031 (`exit` with no argument and EOF both report the last status) also wants. Whichever of the two lands first adds the field; the other reuses it rather than adding a second one. The field is per session like the cwd, never a libc static, so one session's `$?` cannot be seen from another.

Scope is this one parameter. Other special parameters (`$$`, `$#`, `$@`, `$1`) are out: there are no scripts or processes for them to describe.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [x] #1 `echo $?` prints 0 after a command that succeeded and the command's nonzero status after one that failed: `ls /nope` then `echo $?` prints the status ls returned.
- [x] #2 Within one chained line, `$?` reflects the previous item: `false ; echo $?` prints 1 and `true && echo $?` prints 0, once TASK-025 and TASK-030 exist.
- [x] #3 A pipeline sets `$?` to its last segment's status, matching what the line reports to an exec client.
- [x] #4 A syntax error leaves `$?` at 2 and an unknown command leaves it at 127, matching the statuses the shell already returns for them.
- [x] #5 `$?` is 0 at the start of a fresh session, and `echo $?` does not change the value it reads for the next command beyond its own status of 0.
- [x] #6 `${?}` works in the same positions as `$?`; a single-quoted `'$?'` stays literal and a double-quoted `"$?"` expands.
- [x] #7 Each session has its own value: a failure in one session is never visible as `$?` in another.
- [x] #8 history recalls the line as typed, not with the number substituted.
- [x] #9 README and help list `$?` as the one special parameter the shell supports and say the others are unsupported.
<!-- AC:END -->

## Definition of Done
<!-- DOD:BEGIN -->
- [x] #1 task build produces both VPKs
- [x] #2 Verified on a device over a real ssh session, including two concurrent sessions
<!-- DOD:END -->

## Implementation Plan

<!-- SECTION:PLAN:BEGIN -->
1. Tests first (scripts/test_sh_expand.c): move `$?` out of the unsupported list; add checks that `$?` and `${?}` look up the name "?" (quoted, unquoted, glued to text, `\x01?` stays literal), that `$$ $# $@ $* $! $- $0 $1 ${1}` stay unsupported, and add `$?` to the token sweep. Run scripts/test_sh_expand.py to see them fail.
2. src/sh_expand.c parse_ref(): `$?` and `${?}` resolve with name "?" (used 2 / 4); `?` leaves is_special_param's unsupported set. Update include/sh_expand.h comment.
3. Session state: add `int last_status` to struct sh_session (src/sshd_session.c), zero at session creation, with `sh_session_last_status()` / `sh_session_set_last_status()` accessors in include/sshd.h. TASK-031 reuses this field rather than adding another.
4. src/sshd_builtins.c: `expand_lookup()` answers "?" with the session status formatted into the lookup ctx. run_chain stores each item's status after it runs (so `false ; echo $?` sees 1); sh_execute_line stores the final status on every return path, which covers syntax errors (2), unknown command (127), and pipelines (last segment). A skipped `&&`/`||` item leaves it alone.
5. Docs: update the unsupported-syntax message and the `help` text in sshd_builtins.c, README variables paragraph (`$?` supported, others unsupported), docs/ if it mentions it.
6. Verify: host tests, `task build` (both VPKs), then a device run over ssh including two concurrent sessions (see CLAUDE.local.md for the endpoint).
<!-- SECTION:PLAN:END -->

## Implementation Notes

<!-- SECTION:NOTES:BEGIN -->
Device results (the Vita, home network, 2026-11-19, server installed with `vpk install -r`, result.txt `promote package: 0x00000000`): fresh session `echo $?` 0; `ls /nope` then `echo $?` 1; `false ; echo $?` 1; `true && echo $?` 0; unknown command then `echo $?` 127; `echo a | false` then `echo $?` 1; `echo | | x` then `echo $?` 2 and the exec forms `false | true ; echo p=$?` p=0, `true | false ; echo p=$?` p=1; `echo ${?} "$?" '$?' x$?y` printed `0 0 $? x0y`; `false && echo skipped` left `$?` at 1; `echo $?` twice printed 1 then 0; `echo $$` refused with the updated message and `$?` read 2 after; `X=$?` assigned 0; `history` showed every line as typed. Two concurrent sessions: A ran `ls /nope`, B (started after) read `$?` as 0 while A later read 1. `help` prints the `$?` sentence. Host test scripts/test_sh_expand.py passes with the new `$?` and `${?}` checks (they failed before the change).

Observed, not investigated and not touched by this change: a piped, non-pty shell session (`printf ... | ssh host`) did not end after its stdin hit EOF within 40 s in two runs, so the client had to be killed; AGENTS.md says shell sessions finish on EOF. Also, a piped script of more than 8 lines at once hits the documented queue-full refusal.

2026-11-19, at the user's request, the other special parameters were added after this task closed (they were out of scope here): `$$` is a per-session number (`sh_session_id()`), `$0` is `sh`, `$#` is 0, and `$1`.., `$@`, `$*`, `$!`, `$-` are unset; braced forms work, `${10}` included. Positional parameters have no way to be set (no `set --`/`shift`), as chosen by the user. Known deviation: `"$@"` yields one empty word where sh yields none. Host test scripts/test_sh_expand.py extended; build and device run on the Vita (home network): `echo pid=$$ n=$# zero=$0 one=[$1] at=[$@]` printed `pid=2 n=0 zero=sh one=[] at=[]`, and two concurrent sessions printed different `$$` (5 and 6). README, `help` and AGENTS.md updated; the unsupported-syntax message no longer mentions special parameters.
<!-- SECTION:NOTES:END -->

## Final Summary

<!-- SECTION:FINAL_SUMMARY:BEGIN -->
Added `$?` and `${?}`. The session keeps `last_status` (accessors `sh_session_last_status()` and `sh_session_set_last_status()`; TASK-031 reuses it). `sh_expand.c` parses `$?` and `${?}` as the name `?`, and `expand_lookup()` answers it from the session. `run_chain()` stores each item's status as it runs and `sh_execute_line()` stores the final one, which covers syntax errors (2), unknown commands (127), pipelines (last segment) and skipped `&&`/`||` items (unchanged). The other special parameters stay unsupported; the error message, `help`, README and AGENTS.md say so. Verified with the host expand test (new checks failed first), `task build` (both VPKs) and a device run including two concurrent sessions. Follow-up candidate: piped non-pty shell sessions did not end on EOF in the device runs (not caused by this change, not investigated).
<!-- SECTION:FINAL_SUMMARY:END -->
