---
id: TASK-011
title: Give the shell a full-screen text editor over SSH
status: To Do
assignee: []
created_date: '2026-11-16T04:27:00Z'
updated_date: '2026-11-17 03:46'
labels:
  - shell
  - editor
  - server
dependencies: []
references:
  - TASK-010
modified_files:
  - src/sshd_builtins.c
  - src/sshd_session.c
  - include/sshd.h
priority: low
ordinal: 18000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
The server's shell can read and write files (cat, >, >>) but has no way to edit one, so putting an authorized_keys line right or fixing a config means echo gymnastics or carrying the file to a PC and back. The ask: a built-in editor reachable from an SSH session that can open, edit and save a text file on the Vita.

Platform constraints that shape it, from this session's SDK inspection: there is no fork/exec and no ELF loader (same reasons bash cannot be ported), so the editor is a builtin of this shell, not a ported binary; there is no ncurses and no terminfo database, so screen handling is escape sequences written straight to the channel (the line editor already parses CSI sequences and repaints the prompt, so the plumbing exists); and the session has no pty on our side, so the editor owns echo and redraw for its lifetime.

Whether the keybindings are modal (vi-like) or modeless (nano-like) is open — nano needs a visible key legend the channel can render; vi's modal model needs no visible chrome. Pick during implementation and note the choice in the task.

Safety posture that matters more than features: the editor writes the same device surface as the > redirect (data dirs only), and a dropped SSH session mid-edit must leave the file on disk exactly as it was before the edit or exactly as saved — never a partial file.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 `edit FILE` (name during implementation) opens an existing file from a data dir and an empty buffer for a new one, over a plain OpenSSH terminal session.
- [ ] #2 Text can be inserted, changed and deleted on more than one line, with cursor movement and at least one navigation aid (search or goto-line).
- [ ] #3 Saving writes the file back to the Vita; bytes that were never touched compare equal to the original (verified with a binary diff of before/after for an edit touching one line).
- [ ] #4 Losing the SSH connection mid-edit leaves the on-disk file either the original or the last save, never a truncated or half-written file, verified by killing the client mid-edit on hardware.
- [ ] #5 Quitting without saving leaves the file untouched.
- [ ] #6 CRLF files round-trip: line endings are preserved, not normalized.
- [ ] #7 The editor is listed in `help`, and `which edit` finds it.
<!-- AC:END -->
