---
id: TASK-023.01
title: Add a nosleep builtin that controls the idle-timer hold
status: To Do
assignee: []
created_date: '2026-11-17 05:10'
updated_date: '2026-11-18T07:47:00Z'
labels:
  - shell
  - builtin
  - power
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
priority: low
ordinal: 51000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
Add 'nosleep on | off | status'. The behavior already exists; only the command surface is missing.

Current behavior, read it before changing it. src/sshd.c:598-603 holds the device awake: every POWER_TICK_US, if (count_sessions() > 0 || !scePowerIsLowBattery()) it calls scePowerSetIdleTimerCount(0). That call is declared by hand at src/sshd.c:63 because it is not in psp2/power.h, with a comment at :54 explaining why. The comment at :592-597 records the reason the hold is unconditional while connected: the Vita suspended mid-session once and clients saw 'Host is down', and a session dying to standby dies anyway.

DECIDED semantics -- implement this, do not re-litigate it in code:
  on   today's behavior, and the default at launch.
  off  stop holding the device awake ONLY when no session is connected. Concretely: drop the '!scePowerIsLowBattery()' half of the condition and keep the 'count_sessions() > 0' half. A connected session always keeps the device awake; an idle server sitting on a shelf may sleep on the system's own timer.
  rejected: a truly unconditional off. It lets the device sleep mid-session and kills the very session that asked for it -- and the user cannot undo it afterwards, because the session that could type 'nosleep on' is the one that died. Anyone who wants that later needs their own task plus a recovery route that works with no session at all. Say this in the command's own output when 'off' is accepted, so the saving is understood.
  status  report the mode, whether the tick is firing right now, the session count, and scePowerIsLowBattery(). Those are all already available to the poll loop.

Do not persist the mode across launches. A saved 'off' can strand a headless device that nobody can wake, and the default is 'on' anyway. Per-run only, and say so in the usage line.

The state belongs with the server's other run state, not in a session's fsctx -- it is one flag for the whole process, read by the poll loop and written by a builtin, and the poll loop is single-threaded so it needs no lock.

Battery policy must not shift: with no session and a low battery the device still sleeps on its own schedule, and with a session connected it still stays awake regardless of battery. Those are existing behaviors with existing reasoning; treat a change to either as a bug.

Add the row to builtins[] in alphabetical position (between 'mv' and 'pwd').
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 With 'off' and no session connected, the device suspends at the system idle timeout, verified on a device.
- [ ] #2 With 'off' and a session connected, the device stays awake past that same timeout, verified on a device.
- [ ] #3 'on' reproduces today's behavior exactly.
- [ ] #4 Low-battery sleep with no session still works, unchanged.
- [ ] #5 status reports mode, whether the tick is firing, session count and low-battery state, and matches what the device does.
- [ ] #6 The mode resets to on after a relaunch, and the usage text says so.
- [ ] #7 help lists nosleep with its usage, which nosleep reports shell builtin, builtins[] stays alphabetically sorted, and task build is clean.
<!-- AC:END -->
