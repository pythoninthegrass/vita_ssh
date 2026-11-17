---
id: TASK-023.03
title: Add a screen builtin that turns the display off and on
status: To Do
assignee: []
created_date: '2026-11-17 05:10'
labels:
  - shell
  - builtin
  - power
  - display
dependencies: []
references:
  - src/sshd.c
  - docs/vcmd.md
  - 'https://github.com/devnoname120/vitacompanion'
documentation:
  - AGENTS.md
  - docs/vcmd.md
modified_files:
  - src/sshd_builtins.c
  - README.md
  - docs/vcmd.md
parent_task_id: TASK-023
priority: medium
ordinal: 53000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
Add 'screen off | on | status' so a headless server session can put the panel dark instead of burning it for a console nobody watches.

API. scePowerRequestDisplayOff() and scePowerRequestDisplayOn() are both in USER psp2/power.h. ScePower_stub is already linked (CMakeLists.txt:217) and the server already uses this module for the idle tick. Add no libraries.

Do not confuse this with TASK-010. That task wants *dimming*, and its note records that psp2/power.h declares no brightness setter and the public ScePower stub exports no scePowerSetCommonBrightness -- so TASK-010 is still blocked on finding the knob. Turning the panel fully off is a different, already-available call and does not satisfy TASK-010. Do not mark that task done from this one.

Four things this command has to get right, none of which a host build can reveal.

  1. Recovery must be possible over ssh. The server draws its own console with vita2d, so with the display off nobody can read the Vita. 'screen on' has to work from a session while the panel is dark -- verify it on a device rather than assuming.
  2. Restore the display when the server exits. The quit path in src/sshd.c (pad-driven, START held) must call DisplayOn before the app goes away. Shipping a server that can exit with the panel off strands the user in a black UI with no way to see what they are doing.
  3. Off must not become suspend. The poll loop holds the device awake every POWER_TICK_US when a session is connected or the battery is not low (src/sshd.c:598-603). Confirm with the panel off that an idle session survives past the ~4 minute window TASK-010 cites as the old disconnect point.
  4. Record what physically wakes the panel -- a shoulder button, any button, the PS key -- by observing it on a device. Do not guess, because the answer decides whether this command is safe to reach for.

status: there is no documented getter. If none is found, print the mode the server last set and label it as such. Do not invent a readback that looks authoritative.

Also worth checking while in here: whether vita2d keeps presenting frames to a powered-off panel, and whether that costs anything. If the draw loop is pure waste while off, note it for TASK-010 rather than restructuring the loop here.

Add the row to builtins[] in alphabetical position (between 'rm'/'rmdir' and 'stat').
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 screen off darkens the panel and screen on brings it back, both over ssh, verified on a device.
- [ ] #2 Recovery works with the panel already off: 'screen on' from a session restores it without touching the device.
- [ ] #3 Exiting the server while the display is off restores the panel before the app exits, verified on a device.
- [ ] #4 With the panel off and a session connected, an idle session survives past the ~4 minute window that used to be the suspend point.
- [ ] #5 What physically wakes the panel is recorded from observation in docs/vcmd.md.
- [ ] #6 status reports what is actually knowable and labels anything that is only the server's own last-set mode.
- [ ] #7 help lists screen with its usage, which screen reports shell builtin, builtins[] stays alphabetically sorted, and task build is clean.
- [ ] #8 TASK-010 remains open and is not closed by this work.
<!-- AC:END -->
