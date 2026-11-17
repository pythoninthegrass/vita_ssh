---
id: TASK-017.01
title: Build libcurl as a Vita portlib
status: Done
assignee:
  - claude
created_date: '2026-11-17 03:29'
updated_date: '2026-11-17T16:42:00Z'
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
- [x] #1 libs/curl/VITABUILD exists with a pinned pkgver= and a pinned source checksum, and the matching setup:curl task in taskfiles/setup.yml builds the same version from the same source.
- [x] #2 setup:curl builds static only, out-of-tree into a wiped directory, stages into $VITASDK, and fails with a clear message when the expected static library is missing afterwards.
- [x] #3 libcurl is configured against the OpenSSL 1.1.1 Vita port that setup:openssl installs, verified by an assertion that the crypto library resolves inside $VITASDK -- not by trusting configure's own output.
- [x] #4 Host build flags cannot leak in: the recipe scrubs CPPFLAGS, CFLAGS, CXXFLAGS, LDFLAGS and PKG_CONFIG_* before configuring.
- [x] #5 task setup fingerprints the new portlib and skips it when current, and task --force setup:curl rebuilds it, matching how libssh2 and libssh behave.
- [x] #6 A scratch program built with the toolchain resolves a hostname over sceNet and completes one HTTPS GET against a real host on a device, and what it took (resolver choice, CA bundle location and format) is recorded.
- [x] #7 AGENTS.md documents the curl portlib beside the libssh2 and libssh notes, including the two-copies-of-one-recipe rule and anything that turned out to be load-bearing.
- [x] #8 task build still produces both VPKs, with no app linking the new library yet.
<!-- AC:END -->

## Definition of Done
<!-- DOD:BEGIN -->
- [x] #1 task build produces both VPKs
- [x] #2 DNS and TLS probe run on a device and the outcome recorded in AGENTS.md
<!-- DOD:END -->

## Implementation Plan

<!-- SECTION:PLAN:BEGIN -->
1. Version: pin curl 8.17.0, not latest. curl removed OpenSSL 1.1.1 support in 8.18.0 (docs/DEPRECATE.md), and the SDK's port is 1.1.1, so 8.23.0 cannot build against it. 8.17.0 is the last series that still has the 1.1.1 code paths. sha256 955f6e729ad6b3566260e8fef68620e76ba3c31acf0a18524416a185acf77992 (curl-8.17.0.tar.xz, fetched from curl.se).
2. libs/curl/VITABUILD (autotools, like libssh2): --host=arm-vita-eabi, static only, --with-openssl=$VITASDK/arm-vita-eabi, http/https only (disable the rest), no libpsl/zstd/brotli/nghttp2/libssh2 (libssh2 and libssh never mix), no curl tool build unless needed, synchronous resolver first (no threads on a Vita thread).
3. setup:curl in taskfiles/setup.yml mirroring the recipe: pkgver and sha256 read from the VITABUILD, sha check, fresh extract, out-of-tree wiped build, env -u scrub, assert libcrypto resolves in $VITASDK, stage, copy, test -f libcurl.a. Wire into portlibs and taskfile.yml sources (libcurl.a).
4. Scratch probe program under scripts/ or a throwaway dir (not linked into either app): DNS via sceNet plus one HTTPS GET; settle resolver choice and CA bundle location/format. Needs the device: ask Lance which network he is on before running anything against it.
5. Document in AGENTS.md; run task build and confirm both VPKs; check --force and skip behaviour.
<!-- SECTION:PLAN:END -->

## Implementation Notes

<!-- SECTION:NOTES:BEGIN -->
Done so far (2026-11-17): libs/curl/VITABUILD and setup:curl written, wired into setup:portlibs and the build task's sources. task --force setup:curl builds libcurl 8.17.0 (blocking resolver, OpenSSL, zlib, http/https only); a rerun answers 'up to date'; task build still produces client, server and updater VPKs. Cross-compile fixes that were load-bearing: ac_cv_header_stdatomic_h=no (the SDK stdatomic.h is clang's, gcc fails in easy_lock.h), ac_cv_func_pipe2/pipe/eventfd=no and curl_disallow_socketpair=yes (configure's link-only probes find pipe2 and socketpair stubs; socketpair then trips an #error with unix sockets off). Not yet done: on-device DNS/TLS probe (AC #6), AGENTS.md (AC #7). Probe is built at the session scratchpad (probe/b/curl_probe.vpk, title ROM000073) and logs to ux0:data/curl-probe.log; it tries getaddrinfo, gethostbyname, and CURLOPT_CAINFO of the default, vs0:data/external/cert/CA_LIST.cer and ux0:data/cacert.pem. Needs the user to install and launch it on the Vita.
<!-- SECTION:NOTES:END -->

## Final Summary

<!-- SECTION:FINAL_SUMMARY:BEGIN -->
Added libcurl 8.17.0 as a Vita portlib: libs/curl/VITABUILD and setup:curl (mirrored recipe, sha256-checked, scrubbed env, static, http/https only, staged then copied, fails without libcurl.a), wired into setup:portlibs and task build's sources. 8.17.0 because curl 8.18.0 dropped OpenSSL 1.1.1. Verified: task --force setup:curl builds, a rerun skips, task build produces client, server and updater VPKs with nothing linking libcurl. Cross-compile overrides (stdatomic, pipe/pipe2/eventfd, curl_disallow_socketpair) are documented in AGENTS.md. Device probe (work Vita): getaddrinfo and gethostbyname resolve over sceNet; default CA path fails with verify error 20, while vs0:data/external/cert/CA_LIST.cer and curl's cacert.pem both give HTTP 200 from github.com, so CURLOPT_CAINFO is mandatory and the system bundle suffices. The probe source lives only in the session scratchpad. Also updated docs, README, scripts and CLAUDE.local.md for the port 22 default.
<!-- SECTION:FINAL_SUMMARY:END -->
