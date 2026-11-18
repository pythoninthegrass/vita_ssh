---
id: TASK-003
title: Document the libssh portlib and the server's platform rules in AGENTS.md
status: Done
assignee:
  - claude
created_date: '2026-11-15T22:57:00Z'
updated_date: '2026-11-18T07:47:00Z'
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
- [x] #1 AGENTS.md names libssh in the build section alongside OpenSSL and libssh2, with the force-rebuild command and the file that pins its version and digest
- [x] #2 the libssh options that look redundant but are not are recorded with the failure each one prevents, including the one a command-line flag cannot fix without the patch
- [x] #3 the define and the link libraries an app must have to use libssh's server side are stated, with what breaks without them
- [x] #4 the server's platform rules are recorded: per-session cwd instead of the process-global libc one, no printf because there is no stdout or debug screen, callbacks returning zero to accept, one accept per readable-listening-fd event, single-threaded poll loop
- [x] #5 the path-layer rule is recorded: normalize in POSIX space and translate once immediately before the file call, unknown device is an error rather than a guess, no symlink faking
- [x] #6 the platform metadata quirks an agent would otherwise rediscover are written down, including how directories are identified and the absence of a path-length constant
- [x] #7 the manual add_executable rule mentions the new server source files
- [x] #8 an agent with no prior context could add a builtin command without violating any of these rules from AGENTS.md alone
<!-- AC:END -->

## Implementation Plan

<!-- SECTION:PLAN:BEGIN -->
1. Build section: add libssh to the setup order, `task --force setup:libssh`, and the pin (`pkgver=` and `sha256sums=` in libs/libssh/VITABUILD, patch libs/libssh/vita.patch applied on every run).
2. New "libssh portlib" subsection: BUILD_SHARED_LIBS spelling, POSITION_INDEPENDENT_CODE needs vita.patch, HAVE_GCC_THREAD_LOCAL_STORAGE=OFF, env -u scrub + OPENSSL_ROOT_DIR, THREADS_HAVE_PTHREAD_ARG; each with the failure it prevents. Server link contract: WITH_SERVER define and ssh/ssl/crypto/z/pthread plus SceAppMgr/SceCommonDialog stubs, with what breaks.
3. New "Server platform rules" subsection: per-session cwd (not libc chdir), no printf (sh_printf/sh_console_log), callbacks return 0 to accept, one accept per POLLIN, single thread/poll loop, no blocking from callbacks (already covered under SFTP).
4. Path-layer rule (sh_path.h): normalize in POSIX space, translate once, unknown device ENOENT, no symlink faking.
5. Platform metadata quirks: no d_type (stat + S_ISDIR), no PATH_MAX/NAME_MAX without includes, no permission bits (synth mode), usable_time.
6. Update the add_executable bullet to list the server sources; add a "Adding a builtin" recipe.
7. Verify every claim against source, run markdownlint with project config, no hard-wrapping on edited paragraphs.
<!-- SECTION:PLAN:END -->

## Implementation Notes

<!-- SECTION:NOTES:BEGIN -->
Edited AGENTS.md only: Build bullets (libssh, force command, VITABUILD pkgver/sha256sums pin), new sections 'libssh portlib', 'Server platform rules', 'The path layer', 'Platform metadata quirks', 'Adding a builtin command', and the add_executable bullet now lists both source lists. Every claim was checked against CMakeLists.txt, taskfiles/setup.yml, libs/libssh, include/sh_path.h, src/sshd*.c; PATH_MAX/NAME_MAX absence was confirmed by compiling a probe with arm-vita-eabi-gcc. markdownlint -c .markdownlint.jsonc AGENTS.md exits 0. THREADS_HAVE_PTHREAD_ARG=NO has no recorded failure, so the doc says so rather than inventing one. AC #8 is a judgment call and left unchecked for review.
<!-- SECTION:NOTES:END -->

## Final Summary

<!-- SECTION:FINAL_SUMMARY:BEGIN -->
AGENTS.md now documents the libssh portlib and the server's platform rules: build section covers libssh (force command, VITABUILD pin); new sections for the libssh options and link contract, server platform rules, the path layer, platform metadata quirks, and a five-step recipe for adding a builtin. The add_executable bullet lists both source lists. Claims were checked against the sources and a toolchain probe; markdownlint passes. Verification is documentation-only: no build or device run. Risk: THREADS_HAVE_PTHREAD_ARG=NO has no recorded failure and is documented as such.
<!-- SECTION:FINAL_SUMMARY:END -->
