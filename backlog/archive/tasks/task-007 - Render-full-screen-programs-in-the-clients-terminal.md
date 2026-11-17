---
id: TASK-007
title: Render full-screen programs in the client's terminal
status: To Do
assignee: []
created_date: '2026-11-16T00:00:00Z'
updated_date: '2026-11-17 03:46'
labels: []
dependencies: []
documentation:
  - src/text.c
  - include/text.h
  - src/shell.c
  - include/display.h
ordinal: 14000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
src/text.c implements CSI cursor movement, erase, SGR colours and save/restore cursor, and drops every other sequence, including the h and l forms that turn on the alternate screen and a scroll region. Anything that redraws in place — top, vim, a pager's status line — therefore paints over its own previous frame and leaves the screen in a state neither the program nor the user asked for, which is why the README still tells people not to expect interactive programs. The screenshot on the README shows the same class of problem at a smaller scale: neofetch's box-drawing banner comes out as a wall of the same glyph, so font coverage is part of the same gap.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 top draws its display and updates it each refresh without smearing, and q leaves a usable shell prompt
- [ ] #2 entering and leaving the alternate screen restores the screen contents that were there before
- [ ] #3 a program that declares a scroll region scrolls inside that region instead of the whole screen
- [ ] #4 a sequence the terminal does not implement is consumed rather than rendered as its parameters
- [ ] #5 sequences that already work are unchanged: colours, cursor movement, erase, scrollback
- [ ] #6 task build produces both VPKs
<!-- AC:END -->

## Implementation Notes

<!-- SECTION:NOTES:BEGIN -->
2026-11-16: Archived at the user's direction. The client is not being developed further; the sshd server and its shell are the project's primary focus. What stays true of the client and untracked: the renderer in src/text.c still drops the alternate-screen and scroll-region sequences, so a program that redraws in place smears and an unimplemented sequence prints as its parameters, and the README's warning about interactive programs still stands. Full-screen programs over SSH remain tracked where they matter, on the server side, in TASK-011.
<!-- SECTION:NOTES:END -->
