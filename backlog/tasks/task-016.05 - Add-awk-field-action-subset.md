---
id: TASK-016.05
title: Add awk (field-action subset)
status: To Do
assignee: []
created_date: '2026-11-17 03:29'
updated_date: '2026-11-17 03:29'
labels:
  - shell
  - coreutils
dependencies:
  - TASK-015.01
  - TASK-016.01
documentation:
  - AGENTS.md
  - context7 library /holzschu/a-shell
modified_files:
  - src/sshd_builtins.c
parent_task_id: TASK-016
priority: low
ordinal: 39000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
The largest lift in the family, scoped down to what a person types at a prompt: split each line into fields, gate on a pattern, print selected fields. awk '{print $1, $3}' after a pipe is the whole point.

In scope: -F to set the field separator (default whitespace), $0 through the last field, NF and NR, print with comma-separated arguments joined by OFS (a single space by default), and a pattern that gates the action -- either /regex/ or a comparison of a field or NR against a literal with == != < <= > >=.

Explicitly out of scope, refused with awk: unsupported: CONSTRUCT and a nonzero exit rather than being parsed and ignored: variables and assignment, arithmetic, if/while/for, functions, BEGIN/END, and multiple files with different separators. Parsing must fail loudly on anything it does not understand -- an awk that silently drops an unsupported statement reports numbers that look right and are wrong.

Same family rules as the parent task: cmd_awk() in src/sshd_builtins.c, alphabetical row, resolve_arg() for file arguments, sh_printf() for output, sh_eprintf() for errors, heap for the field table, bounded line length and field count.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 awk '{print $1, $3}' FILE prints those two fields of every line joined by a single space, and $0 prints the whole line.
- [ ] #2 -F SEP splits on SEP instead of whitespace, including empty fields between consecutive separators.
- [ ] #3 NF is the field count and NR is the record number, both usable in a pattern and as a print argument.
- [ ] #4 A /regex/ pattern gates the action to matching lines, and a comparison such as $3 == x or NR > 5 gates it the same way.
- [ ] #5 awk reads stdin with no file argument, so ls | awk '{print $1}' works once pipes exist.
- [ ] #6 Unsupported syntax -- assignment, if, while, for, functions, BEGIN, END -- reports awk: unsupported: CONSTRUCT and exits nonzero without printing partial output.
- [ ] #7 A malformed program reports awk: reason with a nonzero exit rather than treating it as a literal string.
- [ ] #8 A line beyond the stated bound for length or field count is reported, not silently truncated.
<!-- AC:END -->

## Definition of Done
<!-- DOD:BEGIN -->
- [ ] #1 task build produces both VPKs
- [ ] #2 Verified on a device over a real ssh session
- [ ] #3 README states which awk subset is supported
<!-- DOD:END -->
