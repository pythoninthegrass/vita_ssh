---
id: TASK-016.02
title: Add head and tail
status: In Progress
assignee: []
created_date: '2026-11-17 03:29'
updated_date: '2026-11-17 05:45'
labels:
  - shell
  - coreutils
dependencies:
  - TASK-015.01
documentation:
  - AGENTS.md
modified_files:
  - src/sshd_builtins.c
  - src/sh_headtail.c
  - include/sh_headtail.h
parent_task_id: TASK-016
priority: medium
ordinal: 36000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
The two simplest members of the family, and the ones that make a long cat survivable on a small screen. Same rules as the parent task: cmd_head()/cmd_tail() in src/sshd_builtins.c, alphabetical rows in builtins[], resolve_arg() for paths, sh_printf()/sh_eprintf() for output and errors.

tail is the only one with a real design question: reading the last N lines of a file means knowing where the end is. Keep it bounded -- stat the file, read backwards in chunks, and refuse with a readable error past a stated size bound rather than loading a huge file into memory. A Vita thread's stack is whatever sceKernelCreateThread was given, so the line accumulation lives on the heap.

Note that tail -f cannot exist: nothing may block in the poll loop, so a follow option is refused with a clear message rather than hanging every session.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [x] #1 head FILE prints the first 10 lines; -n N prints the first N, including -n 0 printing nothing; -c N prints the first N bytes.
- [x] #2 tail FILE prints the last 10 lines; -n N prints the last N; -c N prints the last N bytes.
- [x] #3 Both read the session's stdin when given no file, so cat big | head -n 5 works once pipes exist.
- [x] #4 Several file arguments print the ==> name <== headers between files, as the coreutils convention expects.
- [x] #5 A missing file is reported as head: FILE: No such file or directory with a nonzero exit, and the remaining files are still printed.
- [x] #6 tail refuses -f (follow) with a readable message instead of blocking the poll loop.
- [x] #7 A file beyond tail's stated size bound fails with a readable error naming the bound rather than exhausting memory.
<!-- AC:END -->

## Definition of Done
<!-- DOD:BEGIN -->
- [x] #1 task build produces both VPKs
- [ ] #2 Verified on a device over a real ssh session
<!-- DOD:END -->

## Notes

Shape: the window logic is src/sh_headtail.c (include/sh_headtail.h) -- head_lines,
head_bytes, tail_lines, tail_bytes over one injected seekable reader, no Vita APIs, no
stdout, no paths, so scripts/test_sh_headtail.py runs the exact code the server links,
under ASan/UBSan. src/sshd_builtins.c keeps only options, resolution, and reporting:
ht_command() is the shared parser, cmd_head/cmd_tail are two lines each, and the builtins
table carries the alphabetical rows. Added to the server's add_executable list in
CMakeLists.txt alongside sh_grep.c.

Why a sink rather than an output buffer: SH_HT_SIZE_MAX allows a 16 MB answer and the
server has one thread with every other session waiting on it, so no command may promise
that much heap. sh_head_lines/sh_tail_lines therefore take an optional sink and hand the
answer over SH_HT_CHUNK bytes at a time, and the builtin allocates one chunk. The line
bound is checked as the bytes arrive on that path, not afterwards, because a chunk that
has reached the channel cannot be withdrawn -- verified on the host that a tail whose line
is over the bound prints nothing at all and reports E2BIG, rather than printing the 4096
bytes it had already sent. Without a sink the answer accumulates into out and out_cap is
its bound, which is the shape the host tests use so the reference comparison sees whole
answers.

tail reads backwards. tail_scan() counts newlines down from end, where end is file_size or
file_size - 1 when the last byte is a newline, because that newline ends the last line
rather than opening a blank one; the answer starts one byte after the want'th newline
found and runs to file_size, so it keeps its own trailing newline. The walk is chunked and
pos strictly decreases -- pos is the first byte not yet read, each pass reads
[from, pos) with from < pos, and pos becomes from -- which is the termination argument,
and it is the bug this task spent the most time on: an earlier version reset pos to the
index of the newest newline, which is the same value whenever that newline is the last
byte below the previous pos, and the walk re-read one chunk forever at 100% CPU. That is
why every host binary here now runs under `timeout` and why exit 124 counts as a failure.
The cost of tail -n 20 of a large file is twenty lines of reading plus at most one chunk,
asserted by a 300 KB case that fails if any single read asks for more than a chunk.

Both tools agree with grep on what a line is, because all three use the same definition:
a line ends at a newline or at end of file, the empty file has no lines, "\n" is one empty
line, and a trailing newline belongs to the line it ends. So tail -n 1 of "a\n\n" is the
blank line, not "a", and a file whose last line has no newline prints it unterminated.
Those cases are pinned twice: against a naive reference in scripts/test_sh_headtail.c
written from the specification (not from the implementation), and against real ghead/gtail
in scripts/test_headtail_expectations.py, which checks the expectations themselves rather
than the code. The exhaustive sweep is every string up to nine bytes over {a, b, newline}
for want in 0..5, plus chunk-boundary files at CHUNK-1/CHUNK/CHUNK+1 in three shapes
(terminated, unterminated with a newline at a chunk edge, all newlines).

Bounds. SH_HT_LINE_MAX (4096, the same figure grep uses) is a per-line bound: a wider line
answers E2BIG and is never printed as the prefix that fits, which is what stops head -n 5
quietly becoming head -c 4096. SH_HT_SIZE_MAX bounds file_size and want and is refused
before the first read (ERANGE), reported as "file too large". A sink-less answer larger
than out_cap is ENAMETOOLONG rather than truncated. The sink refusing is ENOMEM. stdin is
pulled whole into a 4 MB buffer because a stream has no seekable offset, and over that it
is refused rather than silently shortened.

Errors go through sh_eprintf in each tool's own voice, never printf; paths only through
resolve_arg(); a directory is "Is a directory"; a failing read keeps the read's errno.
-n 0 prints nothing and exits 0, which is the single deliberate divergence from GNU
(exit 1) and is stated in README.

Size ceiling, and the one thing here that is not about head and tail: the server already
sat at the limit vita-elf-create enforces. Adding the two commands cost 3404 bytes of text
against 3284 of slack, and the failure is an abort -- "Cannot allocate N bytes for SCE data
at end of segment 0; segment 1 overlaps" -- that never says the word size, so it reads as a
broken toolchain. src/sshd_builtins.c is now compiled -Os (set_source_files_properties),
which changes only code layout and buys back more than a command costs; measured clean at
2288558 text with both commands and the header loop in. Removing that line makes the next
builtin added fail the build. Recorded in AGENTS.md, with arm-vita-eabi-size named as the
reading to watch.

Known gaps, deliberately not fixed here. `--` ends the options (so a file named
`-weird` works as `head -- -weird`), but there is no `-e` and a `-n` whose operand begins
with `-` is refused as a bad count; the wording is `head: illegal option -- z`, the same
one `ls` and `grep` use, where the parent task's AC #4 asks for
`name: unsupported option: FLAG` -- no builtin in the family meets that wording yet, so it
is the parent's decision to make, not this one's. `tail -f` is refused rather than
implemented, since it cannot exist in this poll loop. stdin is pulled whole up to 4 MB
rather than streamed, because the walk needs random access, so a producer larger than that
is refused rather than answered shortly. And a `tail -n` answer larger than one chunk goes
out as it is read, so an over-long line discovered in a later chunk is refused after the
earlier chunks were already sent -- the sink-less path the host tests use checks the whole
answer first and prints nothing, and the builtin uses the sink path, which cannot.
