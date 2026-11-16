---
id: TASK-008
title: Let touch drive the client's menus and profile list
status: To Do
assignee: []
created_date: '2026-11-16T00:00:00Z'
labels: []
dependencies: []
documentation:
  - src/main.c
  - src/menu.c
  - include/menu.h
type: task
ordinal: 15000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
The client brings up both touch panels at startup and then uses them for one thing: dragging the terminal and snapping to the bottom in command mode. The main menu, the profile list, the delete confirmation and the IME-launch buttons are d-pad only, on a device whose primary input is the screen. The touch sampling and the selection state already exist, so this is hit-testing that routes into the handlers the buttons use rather than new behaviour.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 tapping a menu row selects it and tapping it again activates it, doing the same thing as the button path
- [ ] #2 tapping a profile connects, and tapping its delete affordance asks for confirmation the same way the buttons do
- [ ] #3 terminal scrolling by touch still works and menu hit-testing does not capture it
- [ ] #4 every existing button still works: touch is additive, not a replacement
- [ ] #5 task build produces both VPKs
<!-- AC:END -->
