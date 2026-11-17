---
id: TASK-017.01
title: Build libcurl as a Vita portlib
status: To Do
assignee: []
created_date: '2026-11-17 03:29'
labels:
  - portlib
  - build
  - network
dependencies: []
documentation:
  - AGENTS.md
  - libs/libssh2/VITABUILD
  - libs/libssh/VITABUILD
  - taskfiles/setup.yml
modified_files:
  - libs/curl/VITABUILD
  - taskfiles/setup.yml
  - taskfile.yml
  - AGENTS.md
parent_task_id: TASK-017
priority: high
ordinal: 40000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
The first new library this project needs. Build libcurl for arm-vita-eabi the way libssh2 and libssh are already built, so the shell can gain curl and wget without hand-rolling HTTP and TLS.

Read libs/libssh2/VITABUILD and libs/libssh/VITABUILD plus their setup: tasks in taskfiles/setup.yml first: the recipe lives in two places at once and they must be edited together, each portlib's version is pinned by pkgver= which the setup task reads, the build is static only and out-of-tree into a wiped directory, staged then copied into $VITASDK, and the task fails loudly if the expected .a is missing afterwards (a configure that quietly found nothing still exits 0). libssh additionally pins sha256sums= and refuses a tarball that does not match -- follow that discipline here too.

Load-bearing details to carry over, all of them already paid for elsewhere in this tree: the env -u CPPFLAGS -u CFLAGS -u LDFLAGS -u PKG_CONFIG_* scrub (a host Homebrew OpenSSL passes the probe and then fails to link against the SDK's 1.1.1 port), pointing the build explicitly at the SDK's OpenSSL rather than trusting discovery, and asserting afterwards in CMakeCache.txt or the config log that the crypto library resolved inside $VITASDK. Bare cmake is never run: task is the entry point, and setup stages into $VITASDK.

The two things most likely to fail on real hardware, which this task has to settle rather than assume: name resolution through sceNet, and where a CA store comes from. Prove both with a scratch program on a device before the builtin task is trusted.

Do not link anything into an app in this task; it is the library, the recipe and the on-device probe.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 libs/curl/VITABUILD exists with a pinned pkgver= and a pinned source checksum, and the matching setup:curl task in taskfiles/setup.yml builds the same version from the same source.
- [ ] #2 setup:curl builds static only, out-of-tree into a wiped directory, stages into $VITASDK, and fails with a clear message when the expected static library is missing afterwards.
- [ ] #3 libcurl is configured against the OpenSSL 1.1.1 Vita port that setup:openssl installs, verified by an assertion that the crypto library resolves inside $VITASDK -- not by trusting configure's own output.
- [ ] #4 Host build flags cannot leak in: the recipe scrubs CPPFLAGS, CFLAGS, CXXFLAGS, LDFLAGS and PKG_CONFIG_* before configuring.
- [ ] #5 task setup fingerprints the new portlib and skips it when current, and task --force setup:curl rebuilds it, matching how libssh2 and libssh behave.
- [ ] #6 A scratch program built with the toolchain resolves a hostname over sceNet and completes one HTTPS GET against a real host on a device, and what it took (resolver choice, CA bundle location and format) is recorded.
- [ ] #7 AGENTS.md documents the curl portlib beside the libssh2 and libssh notes, including the two-copies-of-one-recipe rule and anything that turned out to be load-bearing.
- [ ] #8 task build still produces both VPKs, with no app linking the new library yet.
<!-- AC:END -->

## Definition of Done
<!-- DOD:BEGIN -->
- [ ] #1 task build produces both VPKs
- [ ] #2 DNS and TLS probe run on a device and the outcome recorded in AGENTS.md
<!-- DOD:END -->
