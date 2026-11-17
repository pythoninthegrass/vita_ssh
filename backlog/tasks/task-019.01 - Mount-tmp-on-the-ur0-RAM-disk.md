---
id: TASK-019.01
title: 'Mount /tmp on the ur0: RAM disk'
status: To Do
assignee: []
created_date: '2026-11-17 03:38'
labels:
  - shell
  - path-layer
dependencies: []
documentation:
  - AGENTS.md
  - src/sh_path.c
modified_files:
  - src/sh_path.c
  - include/sh_path.h
  - src/sshd_builtins.c
parent_task_id: TASK-019
priority: medium
ordinal: 44000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
Wanted: /tmp is a real scratch directory that behaves like unix /tmp -- volatile, no persistence promise, cleared by rebooting because ur0: is RAM.

This is the path layer's first prefix rewrite. Everything today maps device-root to device-root (/ux0/a to ux0:a); /tmp/x has to become a directory on ur0: under a different name. sh_path.c is the only file allowed to know that mapping -- no builtin and no SFTP handler may contain the ur0: spelling of a tmp path -- and / being synthetic is the precedent for a name that is not a device.

Decided: ur0: only, no silent fallback to a persistent directory. A fallback would make the same command behave differently depending on free RAM, which is worse than a limit you can read.

The usable size of ur0: is not known to this repository and must be measured on a device rather than guessed at; record the number and how it was measured, the way AGENTS.md expects quantitative claims to be grounded.

The payoff: the tools that need scratch space -- sort's accumulation (TASK-016.03) and curl's partial download (TASK-017.02) -- are pointed here by note on those tasks, so they do not each invent a location. Note that the shell performs no $-expansion, so TMPDIR is for built-in tools to read through the path layer, not something a user types.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 echo x > /tmp/f then cat /tmp/f prints x; ls, rm, cp, mv, stat, head and touch work on /tmp paths with no per-command special casing.
- [ ] #2 The ur0: spelling of /tmp appears nowhere outside src/sh_path.c, checked by searching the tree.
- [ ] #3 cd /tmp && cd .. lands at /, and /tmp/../ux0/data/x resolves to ux0:data/x only via the absolute part: .. clamps at the /tmp root the way it clamps at a device root, never escaping sideways into another device.
- [ ] #4 ls / lists tmp with the devices, and devices or help explains that /tmp is RAM-backed and lost on reboot.
- [ ] #5 env prints TMPDIR=/tmp.
- [ ] #6 A file that would not fit produces a readable error naming /tmp, and the session and every other session keep working afterwards.
- [ ] #7 The measured usable size of ur0: on a device, with the measurement method and date, is recorded in AGENTS.md.
- [ ] #8 Nothing the server needs to survive is written to /tmp: a reboot loses it and no feature breaks.
<!-- AC:END -->

## Definition of Done
<!-- DOD:BEGIN -->
- [ ] #1 task build produces both VPKs
- [ ] #2 Verified on a device: writes work, the reboot clears it, and the full case is readable
<!-- DOD:END -->
