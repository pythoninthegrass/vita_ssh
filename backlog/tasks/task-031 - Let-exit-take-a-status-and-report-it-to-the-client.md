---
id: TASK-031
title: Let exit take a status and report it to the client
status: To Do
assignee: []
created_date: '2026-11-17T18:42:00Z'
updated_date: '2026-11-17T20:45:00Z'
labels:
  - shell
dependencies: []
references:
  - src/sshd_session.c
  - src/sshd_builtins.c
documentation:
  - AGENTS.md
priority: medium
type: enhancement
ordinal: 62000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
`exit` ignores its argument and the session always reports exit status 0 when it ends through `exit`. So `ssh host exit 3` returns 0, and a script cannot use the shell to signal failure the way every POSIX shell lets it. Exec mode already reports the real status of an ordinary command to the client; only the `exit` path is hard-wired to 0. A related gap: when an interactive shell ends because the client sent EOF, the status should be that of the last command, as in sh, rather than a constant.

Wanted: `exit [N]` as POSIX defines it for the cases that apply to a shell with no jobs and no traps.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 `ssh host "exit 3"` returns exit status 3 to the client, and `ssh host exit` with no argument returns the status of the last command run in that session.
- [ ] #2 N is reduced modulo 256 as POSIX specifies, so `exit 256` returns 0 and `exit 257` returns 1.
- [ ] #3 A non-numeric or negative argument prints `exit: ARG: numeric argument required` and ends the session with status 2, as bash and dash do.
- [ ] #4 More than one argument prints `exit: too many arguments`, does not end the session, and exits 1.
- [ ] #5 In a pipeline `exit 3` as a non-last segment is still skipped as in a subshell, and as the last segment ends the session with status 3.
- [ ] #6 An interactive shell session that ends on EOF at an empty line reports the status of the last command it ran.
- [ ] #7 help's usage string for exit shows the optional status, and README states the behaviour.
<!-- AC:END -->

## Definition of Done
<!-- DOD:BEGIN -->
- [ ] #1 task build produces both VPKs
- [ ] #2 Verified on a device over a real ssh session, including a one-shot exec
<!-- DOD:END -->

## Implementation Notes

<!-- SECTION:NOTES:BEGIN -->
2026-11-17: TASK-032 (`$?`) needs the same per-session last-status field this task needs for `exit` with no argument and for EOF. Whichever lands first adds the field in src/sshd_session.c; the other reuses it. TASK-032 depends on TASK-020, so this task will normally land first and should create the field.
<!-- SECTION:NOTES:END -->
