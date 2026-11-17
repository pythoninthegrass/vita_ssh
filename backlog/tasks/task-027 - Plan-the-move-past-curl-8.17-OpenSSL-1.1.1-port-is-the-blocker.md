---
id: TASK-027
title: Plan the move past curl 8.17 (OpenSSL 1.1.1 port is the blocker)
status: To Do
assignee: []
created_date: '2026-11-17T16:44:00Z'
labels:
  - portlib
  - build
dependencies: []
references:
  - AGENTS.md
  - libs/curl/VITABUILD
  - libs/openssl/VITABUILD
  - taskfiles/setup.yml
priority: low
type: chore
ordinal: 58000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
libcurl is pinned at 8.17.0 (TASK-017.01) because curl 8.18.0 removed OpenSSL 1.1.1 support and the SDK's OpenSSL port, installed by setup:openssl, is 1.1.1 (a SonicMastr fork with this repo's entropy patch). 8.17.x gets no new features and will stop getting fixes, and a TLS client with an old curl is the part of this project most exposed to the network. Decide whether to stay on 8.17.x with a documented end date, or move the OpenSSL port to 3.x, which also affects libssh2 and libssh and the entropy patch in libs/openssl/patches. Spike first: nothing here should change until the effort across all three portlibs is known.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 A written recommendation (stay or move) with the cost for openssl, libssh2, libssh and libcurl, recorded in this task.
- [ ] #2 If the answer is move, follow-up tasks exist per portlib; if stay, the support window for 8.17.x is recorded in AGENTS.md.
<!-- AC:END -->
