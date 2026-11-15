---
id: TASK-003
title: Document the libssh portlib and the server's platform rules in AGENTS.md
status: To Do
assignee: []
created_date: '2026-11-15T22:57:00Z'
updated_date: '2026-11-15T22:57:00Z'
labels: []
dependencies:
  - TASK-001.01
documentation:
  - AGENTS.md
  - taskfiles/setup.yml
  - libs/libssh/VITABUILD
  - src/sh_path.c
modified_files:
  - AGENTS.md
type: docs
ordinal: 10000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
AGENTS.md is the first thing an agent reads here and it is accurate but incomplete: it describes an OpenSSL + libssh2 world and says nothing about the libssh recipe that now sits beside it, or about the rules the server app depends on for correctness. The dangerous gaps are the ones an agent will 'clean up' — libssh's option spellings look redundant, the -DWITH_SERVER define looks optional, and a libc chdir() call looks harmless. Each of those is load-bearing, and each cost a debugging cycle to discover. The file already carries this kind of note for libssh2, so this is the same treatment for libssh and for the server sources.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 AGENTS.md names libssh in the build section alongside OpenSSL and libssh2, with the force-rebuild command and the file that pins its version and digest
- [ ] #2 the libssh options that look redundant but are not are recorded with the failure each one prevents, including the one a command-line flag cannot fix without the patch
- [ ] #3 the define and the link libraries an app must have to use libssh's server side are stated, with what breaks without them
- [ ] #4 the server's platform rules are recorded: per-session cwd instead of the process-global libc one, no printf because there is no stdout or debug screen, callbacks returning zero to accept, one accept per readable-listening-fd event, single-threaded poll loop
- [ ] #5 the path-layer rule is recorded: normalise in POSIX space and translate once immediately before the file call, unknown device is an error rather than a guess, no symlink faking
- [ ] #6 the platform metadata quirks an agent would otherwise rediscover are written down, including how directories are identified and the absence of a path-length constant
- [ ] #7 the manual add_executable rule mentions the new server source files
- [ ] #8 an agent with no prior context could add a builtin command without violating any of these rules from AGENTS.md alone
<!-- AC:END -->
