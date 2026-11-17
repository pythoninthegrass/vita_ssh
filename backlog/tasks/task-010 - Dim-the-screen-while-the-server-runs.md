---
id: TASK-010
title: Dim the screen while the server runs
status: To Do
assignee: []
created_date: '2026-11-16T04:00:00Z'
labels:
  - power
  - display
  - server
dependencies: []
documentation:
  - AGENTS.md
modified_files:
  - src/sshd.c
  - include/sshd.h
priority: low
type: enhancement
ordinal: 17000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
The server now keeps the device awake on purpose: the poll loop resets the system idle timer once a second (scePowerSetIdleTimerCount, landed 2026-11-15 after the Vita suspended mid-session and clients saw 'Host is down'). That is correct for reachability but it pins the screen at whatever brightness the system was using, so a device serving for hours on a shelf burns battery through a screen nobody is looking at.

The ask: while the sshd app runs, drive the display below the system's normal brightness, and give the brightness back when the app exits. The screen staying on at all is the documented cost of serving; dimming is how the cost is made bearable.

Feasibility note from this session's SDK inspection, for whoever picks this up: psp2/power.h declares no brightness setter, and strings on libScePower_stub.a shows no scePowerSetCommonBrightness export — so the knob is not in the public ScePower stub and the module that owns it has to be found (or an SDK upgrade supplies it). Do not assume the PSP's sceDisplaySetBrightness path applies.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 On the Vita, with the server running, the display is visibly dimmer than other apps at the same system brightness setting.
- [ ] #2 Quitting the server (START held) restores the brightness the device had before the app launched, verified on hardware.
- [ ] #3 Dimming does not defeat the keep-awake fix: with the screen dimmed, an idle SSH session survives past the old ~4-minute disconnect window, verified on hardware.
- [ ] #4 With no session connected, a low battery still lets the device sleep (the existing low-battery policy still holds).
- [ ] #5 The server's power behavior (stays awake, dimmed screen, low-battery sleep) is documented where the keep-awake tick is documented.
<!-- AC:END -->
