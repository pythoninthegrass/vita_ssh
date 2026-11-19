---
id: TASK-039
title: Expose sshd.log at /var/log/sshd.log through the path layer
status: To Do
assignee: []
created_date: '2026-11-18T23:22:00Z'
updated_date: '2026-11-18T23:24:00Z'
labels:
  - shell
  - path-layer
dependencies:
  - TASK-014
documentation:
  - AGENTS.md
  - include/sh_path.h
  - src/sh_path.c
priority: low
type: enhancement
ordinal: 73000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
Today the log is read as /ux0/data/vitasshd/sshd.log (docs/development.md), which depends on which device the server picked. Wanted: cat /var/log/sshd.log and tail /var/log/sshd.log work the way they do on unix, wherever the log really is.

Why this is not a bolt-on: the path layer's pseudo-file table (SH_TARGET_PSEUDO, e.g. /etc/os-release) is read-only, in memory and has no sce spelling, so it cannot front a real file the server is appending to. The clean route is a prefix remap in src/sh_path.c that maps /var/log onto the server's data directory (<device>:/data/vitasshd, see TASK-014), which is a new concept in the one layer that knows how a POSIX path becomes a device path.

Needs Lance's explicit sign-off before implementation, because AGENTS.md says no symlink faking: decide whether the remap is read-only, whether it covers only sshd.log and sshd.log.old or the whole directory, and whether SFTP sees it.

Points to settle: ls / and ls /var must list it; the --wrap'd libc calls and SFTP must translate it once and never twice (the opendir rule); the remap follows sh_log_path() so it tracks the device the log is actually on; check_writable() behavior for a write to it.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 cat /var/log/sshd.log and tail /var/log/sshd.log print the current log wherever the server put it.
- [ ] #2 ls / lists var and ls /var/log lists exactly sshd.log and sshd.log.old (when present), in POSIX spelling; nothing else in the server directory is reachable through /var/log.
- [ ] #3 A write, rename or removal under /var/log, or a redirect into it, is refused with a readable read-only error.
- [ ] #4 The remap resolves through sh_path.c only; no builtin builds the device path by hand, and a path is never translated twice.
- [ ] #5 SFTP lists and reads /var/log/sshd.log (and refuses writes) on a device, through the same remap.
- [ ] #6 README and docs/development.md name /var/log/sshd.log as where to read the log.
<!-- AC:END -->

## Implementation Notes

<!-- SECTION:NOTES:BEGIN -->
Design settled (Lance, 2026-11-18): the remap is read-only; it covers only the log files (sshd.log and sshd.log.old), not the whole server directory; SFTP sees it. Sign-off requirement from the description is met.
<!-- SECTION:NOTES:END -->
