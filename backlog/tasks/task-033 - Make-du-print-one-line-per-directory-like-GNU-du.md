---
id: TASK-033
title: Make du print one line per directory like GNU du
status: To Do
assignee: []
created_date: '2026-11-17T22:09:00Z'
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
- [ ] #1 `du PATH` prints one line per directory in the tree, each child before its parent, with the PATH argument's own total as the last line
- [ ] #2 Each line is `<size><TAB><path>` with the size in 1K units, computed from st_size rounded up to 1K (a 1-byte file counts 1, an empty directory 0)
- [ ] #3 The printed path is the argument as typed with the sub-path appended (`du .` prints `./src`; `du /ux0/data` prints `/ux0/data/x`), not the resolved POSIX path
- [ ] #4 `-s` prints only the argument's total line, `-h` prints human-readable sizes (`254M`), `-a` also prints a line per file, `-k` is accepted and is the default; `-sh` combines
- [ ] #5 With no argument du walks the current directory and prints `.` paths
- [ ] #6 A directory that cannot be opened or a file that cannot be stat'd prints `du: <path>: <strerror>` to stderr, the walk continues, and the status is 1
- [ ] #7 A dedicated test or device check compares the output shape against a known small tree and records the result in the task notes
- [ ] #8 The walk still yields to the poll loop (sh_session_yield) and still stops on sh_session_interrupted
- [ ] #9 `du /ux0 | wc -l` on a device either completes or fails with the existing `output too large for a pipe` message; the outcome is recorded in the task notes
- [ ] #10 The `du` row in the help table and any docs that describe du say what it prints and the st_size rounding
<!-- AC:END -->
