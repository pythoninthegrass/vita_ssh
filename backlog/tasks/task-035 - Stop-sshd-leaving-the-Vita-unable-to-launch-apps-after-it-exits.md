---
id: TASK-035
title: Stop sshd leaving the Vita unable to launch apps after it exits
status: Done
assignee:
  - Claude
created_date: '2026-11-18T05:42:00Z'
updated_date: '2026-11-18T22:57:00Z'
labels:
  - sshd
  - power
dependencies: []
references:
  - src/sshd.c
  - 98a8fc1
modified_files:
  - src/sshd.c
priority: high
type: bug
ordinal: 66000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
Commit 98a8fc1 made the server take two kernel power locks (disable auto-suspend, disable OLED off) while serving, and release them only when the poll loop returns normally. Quitting from the PS menu kills the process before that runs. On a device (work Vita, 2026-11-17, card verified clean by fsck_exfat), running sshd once and then switching to another app left every later app launch hanging on the LiveArea start animation with a cancel sign, VitaShell included, until a force reboot. sshd.log shows both locks taken (rc 0x0) on every start and no unlock line on any exit. The build immediately before that commit (462f824), with only the idle-timer reset, quit and relaunched sshd and VitaShell repeatedly with no hang. The lock change was added because the idle-count reset alone let the panel turn off, so the panel-off behavior it addressed must be re-checked after the lock is removed. A corrupt exFAT bitmap on the card (repaired) was a separate, earlier problem that confused the first runs. TASK-023.01 and TASK-023.03 also touch the power hold and should not assume the kernel locks exist.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [x] #1 After running sshd once and quitting it from the PS menu, VitaShell and other apps launch normally, repeated at least three times on a device.
- [x] #2 The server takes no kernel power lock that outlives its process; sshd.log shows no sceKernelPowerLock call.
- [x] #3 While serving, the device does not auto-suspend and WiFi stays up for as long as it did before 98a8fc1.
- [x] #4 What the device does with the panel while serving (turns off, dims or stays on) is recorded from a device run, so the reason 98a8fc1 was made is not lost.
- [x] #5 The log and code comments describe the power behavior as it now is, with no mention of the removed locks as current behavior.
<!-- AC:END -->

## Implementation Plan

<!-- SECTION:PLAN:BEGIN -->
Researched src/sshd.c at 6fb5c0e: power_hold() takes sceKernelPowerLock for DISABLE_AUTO_SUSPEND and DISABLE_OLED_OFF, called from server_loop() every POWER_TICK_US and released by power_hold(0) after server_loop() returns; power_tick() resets the idle count and calls sceKernelPowerTick for the same two types.

1. Remove power_hold(), power_locked and both call sites; keep power_tick() (idle-count reset plus sceKernelPowerTick) so the tick still runs once a second while there is a reason to hold the device awake. A tick leaves nothing to release when the process is killed.
2. Rename POWER_HOLD_TYPES / power_hold_types to say they are tick types, and fix the comment and the startup log line so neither mentions locks.
3. Build with `task build`; compare arm-vita-eabi-size before and after.
4. Commit src/sshd.c alone, conventional commit, no attribution.

Risk: the build verified on a device is 462f824 (idle-count reset only, no sceKernelPowerTick). The tick-only build is not yet verified: sceKernelPowerTick for DISABLE_AUTO_SUSPEND and DISABLE_OLED_OFF is a new call in the verified-bad build too, so a device run after the change must repeat the quit and relaunch test before AC1 is checked. If it still hangs, fall back to the idle-count reset alone and record the panel behavior.
<!-- SECTION:PLAN:END -->

## Implementation Notes

<!-- SECTION:NOTES:BEGIN -->
2026-11-18: removed power_hold() and both call sites in src/sshd.c; power_tick() (idle-count reset plus sceKernelPowerTick) stays. task build produces all VPKs; server text 2297512 bytes (2297824 with the locks and TASK-030's builtins, 2297612 at 6fb5c0e). The working-tree server VPK also carries TASK-030's true/false/: builtins, sha256 f654a7a2cd87784c156dbf0a3333d389515ba84d3e581b21ab7a574be619733b. Not yet run on a device. The tick-only behavior is unverified: the device-verified good build is 462f824 (idle-count reset only). Nothing committed.

2026-11-17 device run (work Vita, card clean per fsck_exfat, installed eboot.bin byte-identical to the working-tree build): user switched between the tick-only sshd and VitaShell several times with no hang; sshd.log shows three starts (23:43:50, 23:44:01, 23:44:12) with 'power: idle timer reset and power ticks 1/s' and no lock lines. AC3 (no suspend, WiFi stays up over time) and AC4 (panel behavior while serving) were not observed and stay open.

2026-11-18: `vpk install` over ssh succeeded on the tick-only build (updater staged and promoted, result.txt 'promote package: 0x00000000', server back on its own). The failed handoff on 2026-11-17 ran while the leaked power locks were held; whether the locks caused that failure is a guess, not shown.

2026-11-18: the tick-only change is committed as b8b3d14 (the 'Nothing committed' note above is stale); the working tree is clean at cf0f825 and src/sshd.c has no sceKernelPowerLock call. Open: AC3 and AC4 need an unattended device run, because the panel and the suspend timer can only be watched on the Vita.

2026-11-18 device run (office Vita, tick-only build): served untouched for 35 minutes, 5 minutes past the device's sleep option, and ssh still worked; the Mac's nc -z -w 3 to port 22 also succeeded at 16:40 local. No auto-suspend, WiFi stayed up. AC3 checked. Panel behavior (AC4) was not reported and stays open.

2026-11-18 panel behavior while serving (same 35 minute run, office Vita, tick-only build): the panel stayed on and dimmed, and did not turn off. That is the behavior the user wants, so the OLED-off lock that 98a8fc1 added is not needed.
<!-- SECTION:NOTES:END -->

## Final Summary

<!-- SECTION:FINAL_SUMMARY:BEGIN -->
Removed power_hold() and its two sceKernelPowerLock calls from src/sshd.c (b8b3d14). The server now resets the idle count and calls sceKernelPowerTick for auto-suspend and OLED-off once a second while serving, so nothing is left held when the PS menu kills the process. The startup log line and the code comment describe the tick behavior. Device results: running sshd and VitaShell back and forth several times left no launch hang (2026-11-17, no lock lines in sshd.log); a 35 minute untouched run, 5 minutes past the sleep option, did not suspend and kept WiFi and ssh up; the panel stayed on and dimmed rather than turning off, which is the wanted behavior. Follow-up: TASK-023.01 and TASK-023.03 touch the power hold and must not assume kernel locks exist.
<!-- SECTION:FINAL_SUMMARY:END -->
