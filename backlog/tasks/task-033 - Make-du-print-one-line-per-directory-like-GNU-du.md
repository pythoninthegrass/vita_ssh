---
id: TASK-033
title: Make du print one line per directory like GNU du
status: Done
assignee:
  - Claude
created_date: '2026-11-17T22:09:00Z'
updated_date: '2026-11-19T22:32:00Z'
labels:
  - shell
  - sshd
dependencies: []
references:
  - src/sshd_builtins.c
  - AGENTS.md
priority: medium
type: enhancement
ordinal: 64000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
`du` on the Vita prints a single human-readable total per argument (`2.2G  /ux0`), so it cannot show where space went, and scripts written for GNU du (`du | sort -n`, `du -sh`) get the wrong shape. The user supplied GNU du output as the reference: one line per directory, children listed before their parent, sizes in 1K units, paths as typed (`./src`, `./.git/hooks`), the argument's own total last, and `du -sh` giving `254M	.`.

Decided with the user (2026-11-17): the size of a file is its `st_size` rounded up to 1K. This is apparent size, not disk usage; the Vita's FAT `stat()` has no block count and no cluster-size lookup is wanted, so the figures will not match a desktop du's cluster-rounded numbers (e.g. the 4 shown for tiny files). Separate from TASK-028, which only made the walk yield to the poll loop.

A walk over a big card now emits one line per directory, so output into a pipe (`du /ux0 | wc -l`) is bounded by the 1 MB pipe limit and the walk keeps yielding to the poll loop as TASK-028 established.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [x] #1 `du PATH` prints one line per directory in the tree, each child before its parent, with the PATH argument's own total as the last line
- [x] #2 Each line is `<size><TAB><path>` with the size in 1K units, computed from st_size rounded up to 1K (a 1-byte file counts 1, an empty directory 0)
- [x] #3 The printed path is the argument as typed with the sub-path appended (`du .` prints `./src`; `du /ux0/data` prints `/ux0/data/x`), not the resolved POSIX path
- [x] #4 `-s` prints only the argument's total line, `-h` prints human-readable sizes (`254M`), `-a` also prints a line per file, `-k` is accepted and is the default; `-sh` combines
- [x] #5 With no argument du walks the current directory and prints `.` paths
- [x] #6 A directory that cannot be opened or a file that cannot be stat'd prints `du: <path>: <strerror>` to stderr, the walk continues, and the status is 1
- [x] #7 A dedicated test or device check compares the output shape against a known small tree and records the result in the task notes
- [x] #8 The walk still yields to the poll loop (sh_session_yield) and still stops on sh_session_interrupted
- [x] #9 `du /ux0 | wc -l` on a device either completes or fails with the existing `output too large for a pipe` message; the outcome is recorded in the task notes
- [x] #10 The `du` row in the help table and any docs that describe du say what it prints and the st_size rounding
<!-- AC:END -->

## Implementation Plan

<!-- SECTION:PLAN:BEGIN -->
1. Pure helpers in new `src/sh_du.c` / `include/sh_du.h` (`sh_du_kb()` rounds st_size up to 1K; `sh_du_human()` GNU-style -h: ceil, one decimal under 10, K/M/G/T), with a host test `scripts/test_sh_du.py` + `.c` under ASan/UBSan, written first and seen failing.
2. Rewrite `du_tree()`/`cmd_du()` in `src/sshd_builtins.c`: a context struct holding flags (-a -s -h -k), the as-typed path buffer (append/truncate per level), the walk scratch and the status. Children print before their parent; per-file KB sums; directory itself counts 0. Errors `du: <typed path>: <strerror>` to stderr, walk continues, status 1. Keep `sh_session_yield` and `sh_session_interrupted`; an interrupted walk prints nothing further. `-s` with `-a` is refused as GNU does.
3. Update the `du` help row, README/docs mentions, `scripts/smoke.sh` (compare shape against a known tree).
4. `task build`, host tests, then device: install with `vpk install -r`, run shape check, `du /ux0 | wc -l`; record outcomes in notes.
<!-- SECTION:PLAN:END -->

## Implementation Notes

<!-- SECTION:NOTES:BEGIN -->
Host: `python3 -I scripts/test_sh_du.py` (new, src/sh_du.c under ASan/UBSan) written first and seen failing (missing source), then passing. It covers the 1K round-up and the -h text.

Device (the Vita, home network, 2026-11-19, after `vpk install -r`, result.txt `promote package: 0x00000000`): tree with sub/f.txt (6 bytes) and an empty dir. `du` printed `1 ./sub`, `0 ./empty`, `1 .`; `du -a DIR/` printed the file line, then sub, empty, then `DIR/` with the trailing slash kept; `du -sh .` gave `1.0K .`; `du -s sub empty` and `du -k sub/f.txt` as expected; `du -as` refused (rc 1), `du -z` refused (rc 1), `du nosuch sub` printed the error, still walked `sub` and exited 1, `du /` is `Invalid argument`. scripts/smoke.sh exec grew three du shape checks, all PASS.

AC9: `du /ux0 | wc -l` completed on the device, 953 lines in 2m51s, no pipe-limit message. `du -sh /ux0` gave 2.3G and the last line of `du /ux0` was 2395182 (KB), consistent.

AC6: the missing-argument path was exercised on the device; the opendir-failure and per-entry stat-failure messages are source-only, because there is no unreadable directory to arrange on the card.

Syntax learned while testing: a `;` must be its own word, and a line holds at most 16 commands.

Unrelated: smoke.sh `var: unsupported syntax exits 2 (echo $1)` fails (0, want 2) because TASK-032 made `$1` a supported special parameter; the check is stale and was left alone.

AC6 on a device (the Vita, work network, 2026-11-19): the empty game card slots fail opendir, so `du -s /gro0 ; echo rc=$?` and the same for `/grw0` printed `du: /gro0: Not a directory`, then `0\t/gro0`, and rc=1. That confirms the opendir-failure message, the walk carrying on to print the total, and status 1. The other device roots (`/tm0`, `/ud0`, `/uma0`, `/pd0`, `/sa0`, `/vd0`) walked clean with rc=0. The per-entry stat-failure branch in `du_tree()` is still source-only (no entry that lists but cannot be stat'd could be arranged); it uses the same message form and `failed` flag as the opendir branch.

Syntax slip while testing: `echo rc=$?` glued to a `;` (`du -s /x; echo`) is read as arguments to du, so the `;` must be its own word.
<!-- SECTION:NOTES:END -->

## Final Summary

<!-- SECTION:FINAL_SUMMARY:BEGIN -->
`du` now prints one line per directory in GNU order (children before parent, argument total last), sizes in 1K units from st_size rounded up, paths as typed, with `-a`, `-s`, `-h` and `-k`. The size and -h helpers live in `src/sh_du.c` with a host test (`scripts/test_sh_du.py`, ASan/UBSan). `scripts/smoke.sh exec` has three du shape checks that pass. On a device, `du /ux0 | wc -l` completed with 953 lines in 2m51s with no pipe-limit message, and `du -sh /ux0` agreed with the last line of `du /ux0`. Error handling was checked on the empty game card slots, which fail opendir: the message is printed, the walk continues and the status is 1. Not exercised on hardware: the per-entry stat-failure branch, which is source-only. The stale smoke.sh `echo $1` check is a separate item (TASK-042).
<!-- SECTION:FINAL_SUMMARY:END -->
