---
id: TASK-016.03
title: 'Add wc, sort and uniq'
status: Done
assignee: []
created_date: '2026-11-17 03:29'
updated_date: '2026-11-17 09:30'
labels:
  - shell
  - coreutils
dependencies:
  - TASK-015.01
documentation:
  - AGENTS.md
modified_files:
  - src/sshd_builtins.c
parent_task_id: TASK-016
priority: low
ordinal: 37000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
The pipeline companions: wc counts, sort orders, uniq collapses adjacent duplicates. Without them a pipe can only be read by eye. Same rules as the parent task -- cmd_wc()/cmd_sort()/cmd_uniq() in src/sshd_builtins.c, alphabetical rows, resolve_arg(), sh_printf()/sh_eprintf(), no printf, no chdir, no invented device paths.

sort is the memory-sensitive one: it must accumulate everything before it can emit anything, so it needs a stated bound with a readable error past it (the exec-output bound in TASK-002.03.02 is the precedent for that style of limit), and the accumulation belongs on the heap. Comparison is byte order (C locale), stated in help.

uniq collapses only adjacent duplicates -- say so in the usage string rather than letting people assume it deduplicates a whole file.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [x] #1 wc FILE prints lines, words and bytes in that order; -l, -w and -c print only their own column; several files print one row each plus a total row.
- [x] #2 wc reads stdin with no file argument, so wc -l after a pipe counts what the pipe produced.
- [x] #3 sort FILE orders lines by byte value; -n sorts numerically; -r reverses; -u drops duplicate lines after sorting.
- [x] #4 sort with -t or -k refuses with a readable unsupported-option message and a nonzero exit rather than ignoring them.
- [x] #5 sort past its stated memory bound reports a readable error naming the bound and leaves the session usable.
- [x] #6 uniq collapses adjacent duplicate lines; -c prefixes a count; -d prints only lines that were duplicated; -u prints only lines that appeared once.
- [x] #7 All three read stdin when given no file argument, and all three report a missing file as name: FILE: reason.
- [x] #8 help shows a one-line usage for each that names the subset it implements.
<!-- AC:END -->

## Implementation Notes

<!-- SECTION:NOTES:BEGIN -->
Scratch space: when TASK-019.01 lands, sort's accumulation spool resolves through the path layer to /tmp (the ur0: RAM disk) rather than a hand-picked directory, and the memory bound reported in the error should name the same limit the tool actually enforces.
<!-- SECTION:NOTES:END -->

## Definition of Done
<!-- DOD:BEGIN -->
- [x] #1 task build produces both VPKs
- [x] #2 Verified on a device over a real ssh session
<!-- DOD:END -->

## Device run

Run on the home Vita over real ssh exec sessions, one ssh at a time with the system /usr/bin/ssh, comparing the Vita's wc, sort and uniq with gwc, gsort and guniq (LC_ALL=C) on exact bytes and exit status over eight fixtures (duplicates, no trailing newline, empty, blank lines, numeric edge lines, words with tabs, a 20000-line 175 KB file, adjacent runs). The reviewer wrote these notes because the implementing agent made no task-file commit.

The build the agent delivered (5a6eb69) passed 147 of 163 cases. The 16 failures were two defects: wc -l and wc -w on a single file printed one byte more than GNU because the shared column width counted the unrequested bytes column, and sort and uniq on the 20000-line file timed out at 40 s with partial output because each output line was up to three separate channel writes. Everything else matched, including sort -n, -r, -u, -nu and -ru, every uniq mode, and the multi-file wc total rows. That run had no transport resets.

After commit 9a6330a (wc width is the digit count of the total byte size, and no padding for one column of one input) and commit 55e60db (sort, uniq and grep output batched into one 2048-byte buffer, flushed before any error message and at the end) the comparison passed 161 of 163; the two misses were sort and uniq -u on the 175 KB file hitting the 40 s timeout mid-output, and both pass alone in 1 to 2 s. sort of that file takes 2 s against 1 s for cat. After commit 6bc454e the final build passed 162 of 163, the miss being a `Connection reset by peer` partway through the 334 KB output of uniq -cu, which passed on the first retry in 2 s.

Other device checks: stdin through `<` matches GNU for sort and uniq in every tested mode (20 of 20) and, after 6bc454e, wc on stdin matches gwc exactly (49 of 49: a bare count for one column, otherwise the width comes from the byte count; a pipe will need a second look under TASK-015.02 because GNU pads a non-regular stdin to seven). A missing file prints `NAME: FILE: reason`, the other files are still processed and the exit is 1, with wc output and its total row identical to gwc; GNU's sort exits 2 for a missing file where this one exits 1. sort -t, -k and -s, wc -m and -L, and uniq -i and -f exit 2 with `illegal option -- X`, which is the wording ls, grep, head and tail use; the parent TASK-016 AC #4 asks for `name: unsupported option: FLAG`, a conflict that predates this task and is the parent's to settle. help prints one usage line for each, and uniq says adjacent.

Bounds, using scratch files that were removed from the card afterwards: a 5 MB file is refused by sort, uniq and wc with the 4194304-byte limit named; 200001 lines of `a` is refused by sort and uniq streams it; exactly 200000 lines sorts (400000 bytes out in 4 s); a 70000-byte line is refused by sort and uniq with `line too long (limit 65536 bytes)` while wc counts it. The sort and uniq error for a line-count overrun used to name only the byte limit; 6bc454e makes it name both (`input too large (limit 4194304 bytes or 200000 lines)`). After every refusal the next command in a fresh session answered, which is how session usability was checked. Known limitation: wc also refuses files over 4 MiB although it could stream them.

Size ceiling: the server text was 2295052 at the agent's tip and is 2295544 now; both VPKs build and the `Cannot allocate N bytes for SCE data` abort did not occur. There is essentially no headroom left for the next builtin.

About the implementing run, for the record: a Python splice corrupted scripts/test_sh_textops.c and the agent rebuilt the roughly 490-line tail from the head, and said so. A mutation check by the reviewer (reversing the byte comparison in byte_cmp) makes the test fail with 113941 failing lines, so the rebuilt test does bite. The agent left a stray import-order edit to scripts/test_vpk_updater.py and a stray empty file named stdout; both were gone before its commits. It disregarded the explicit instruction to avoid multi-line comments: about 54 blocks remain (23 in src/sh_textops.c, 9 in include/sh_textops.h, 14 in scripts/test_sh_textops.c, 8 added to src/sshd_builtins.c).

Transport, not attributed to any command here: across all device runs the Vita shows connection resets and hung sessions at roughly 1 percent of calls for any command including cat, and /ux0/data/vitasshd/sshd.log records poll-loop stalls of 3 to 40 s, with `transport closed while output waited for room` during outputs of several hundred KB. Two reviewer errors are also on record: my first comparison script had a loop-variable bug that corrupted the file names, and I discarded that log.

