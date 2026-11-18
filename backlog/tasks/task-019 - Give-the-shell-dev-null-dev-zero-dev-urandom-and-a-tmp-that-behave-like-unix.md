---
id: TASK-019
title: >-
  Give the shell /dev/null, /dev/zero, /dev/urandom and a /tmp that behave like
  unix
status: To Do
assignee: []
created_date: '2026-11-17 03:38'
updated_date: '2026-11-18T07:47:00Z'
labels:
  - shell
  - path-layer
dependencies: []
documentation:
  - AGENTS.md
  - src/sftp_server.c
  - src/sh_path.c
modified_files:
  - src/sh_path.c
  - include/sh_path.h
  - src/sftp_server.c
  - src/vita_shims.c
  - CMakeLists.txt
priority: medium
ordinal: 43000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
Both names answer ENOENT today, and that is deliberate: an unknown first component is ENOENT, with no fallback to a default device, because a fallback turns a typo into a file on the wrong device. This task makes the two exceptions everyone expects, and only these two.

Already available, and the reason this is feasible at all: / is already a synthetic entry in the path layer (SH_TARGET_ROOT, a listing of the device table rather than a directory), so a path that is not a device has precedent. ur0: is already in the device table marked writable (src/sh_path.c) and sshd_hostkey.c already depends on it being writable and volatile -- RAM-backed, forgotten on reboot -- which is /tmp semantics exactly. And --wrap=open plus --wrap=stat are already on the server's link line, with __wrap_open() falling through to __real_open() whenever no SFTP session is active -- which is every call the builtin shell makes. Every open() in the server binary already passes through a function of ours.

Decisions already made with the user, do not re-litigate: /tmp is the ur0: RAM disk, not a persistent directory on ux0: (a /tmp that survives a reboot is scratch space wearing the wrong name); /dev supplies null, zero and urandom.

Two hazards to respect, both from rules already paid for in AGENTS.md:
1. --wrap rewrites calls made inside libc itself, which is what produced the opendir double-translation bug. Synthetic devices must be recognized by file descriptor, never by re-translating a path string, and any new wrapper that calls into libc has to ask whether libc calls another wrapped function on its way through.
2. normalise_stat() in src/sftp_server.c flattens every entry to a plain file or directory. The /dev entries have to reach SFTP as character devices ahead of that flattening, or sftp ls -l /dev reports regular files.

Keep the surface honest: rm, mkdir, chmod, chown and rename against a /dev path refuse with a readable error. There are no symlinks on the Vita and nothing here pretends otherwise.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 cat /dev/null succeeds and prints nothing; grep PATTERN /dev/null exits 1 with no output; echo hi > /dev/null succeeds and creates no file anywhere.
- [ ] #2 ls -l /dev lists null, zero and urandom as character devices, over the shell and over sftp alike, and stat on them says so instead of reporting a regular file.
- [ ] #3 A file created in /tmp is readable for the rest of the session and is gone after a reboot; touch, ls, rm, cp and mv all work there with no per-command special casing.
- [ ] #4 ls / lists dev and tmp alongside the devices, so both are discoverable; env reports a TMPDIR pointing at /tmp.
- [ ] #5 Writing until /tmp is full produces a readable error naming /tmp, not a bare sceIo code and not a hang.
- [ ] #6 rm on a /dev path, mkdir inside /dev, and cd into a /dev file all refuse with a readable error and a nonzero exit.
- [ ] #7 Every path that is not /, /dev or /tmp still resolves exactly as before, including ENOENT for an unknown first component: these are the only synthetic names that exist.
<!-- AC:END -->
