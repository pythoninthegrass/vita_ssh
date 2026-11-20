---
id: TASK-016.09
title: Add capture groups to the grep engine and back-references to sed
status: To Do
assignee: []
created_date: '2026-11-20T18:06:00Z'
labels:
  - shell
  - coreutils
dependencies:
  - TASK-016.04
modified_files:
  - src/sh_grep.c
  - include/sh_grep.h
  - src/sh_sed.c
  - scripts/test_sh_grep.c
  - scripts/test_sh_sed.c
  - scripts/test_sh_sed.py
  - README.md
  - docs/shell-text-tools.md
parent_task_id: TASK-016
priority: low
type: feature
ordinal: 79000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
sed's `\1`..`\9` in a replacement is refused today because the grep matcher (src/sh_grep.c, a Thompson simulation) tracks no capture positions, so it can report only where a whole match is (`sh_grep_span()`), not where each `\( \)` group matched. Swapping `s/\(a*\)b/\1/` for a literal 1 would be a silent difference, hence the refusal. This task adds group spans to the engine and then lets the replacement use them.

Open design question to settle first: a pure NFA simulation can carry capture slots per live state (cost grows with groups times states, and the step and memory bounds must still hold), or captures can be recovered after the fact by re-matching the winning span with a backtracking pass over the already-found [so,eo). The leftmost-longest overall rule must stay identical to GNU, and POSIX subexpression rules (leftmost-longest per group) are subtler than the whole-match rule. Back-references inside the pattern (`\(a\)\1`) are a separate, larger feature and out of scope unless the design makes them free; grep would gain them too, which changes its documented syntax.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 The engine reports the start and end of each \( \) group (up to GROUP_MAX) for the leftmost-longest match, with an unmatched group reported as such.
- [ ] #2 sed accepts \1 to \9 in a replacement, refuses a number above the group count of its pattern with a message naming it, and gives GNU's answer for worked examples including a group inside a repeat and an unmatched alternative.
- [ ] #3 The step budget, live-state and memory bounds still hold; scripts/test_sh_grep.py keeps its no-blowup checks and gains a model-checked capture sweep.
- [ ] #4 scripts/test_sh_sed.py pins capture cases against gsed; the README sed paragraph and docs/shell-text-tools.md describe what is supported and drop the back-reference refusal.
<!-- AC:END -->

## Definition of Done
<!-- DOD:BEGIN -->
- [ ] #1 Verified on a device over a real ssh session
<!-- DOD:END -->
