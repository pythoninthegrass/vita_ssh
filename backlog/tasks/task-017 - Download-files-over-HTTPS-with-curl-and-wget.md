---
id: TASK-017
title: Download files over HTTPS with curl and wget
status: Done
assignee: []
created_date: '2026-11-17 03:28'
updated_date: '2026-11-19T03:13:00Z'
labels:
  - shell
  - network
  - portlib
dependencies: []
references:
  - 'https://github.com/juanjsebgarcia/vita-turbo-wifi/releases/tag/v0.7.2'
documentation:
  - AGENTS.md
modified_files:
  - libs/curl/VITABUILD
  - taskfiles/setup.yml
  - src/sshd_builtins.c
  - CMakeLists.txt
priority: high
ordinal: 31000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
Wanted: from an SSH session, cd ~/Downloads then curl -LJO the wifiturbo.suprx release asset leaves that file in ~/Downloads with the release's name and size. wget over the same engine afterwards.

Why it is more than one builtin: the client links libssh2 and the server links libssh, both built from a recipe that lives in two places at once (libs/<name>/VITABUILD and the matching setup: task in taskfiles/setup.yml), both configured against the OpenSSL 1.1.1 Vita port that setup:openssl installs. curl is the first thing here that needs a new portlib, and on Vita the two things most likely to fail are DNS through sceNet and the TLS CA store -- both have to be settled on a device, not on a host build.

Agreed approach: libcurl as a portlib, not a hand-rolled HTTP client, so wget and any later fetch share one transfer implementation. The portlib task is build-system work plus the on-device DNS/TLS probe; the builtin task is shell work on top.

Acceptance asset: https://github.com/juanjsebgarcia/vita-turbo-wifi/releases/download/v0.7.2/wifiturbo.suprx

Depends on the home-directory task for ~/Downloads; the portlib itself does not.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [x] #1 curl and wget are both reachable from the shell and documented in README with the flags that work.
- [x] #2 The GitHub release asset above downloads end to end on a device, into ~/Downloads, with the correct filename and byte count.
- [x] #3 HTTPS verifies certificates against a real CA store by default; verification is off only with an explicit insecure flag that says so on the terminal.
- [x] #4 A download cannot park the poll loop: a stalled or very slow server hits a bounded timeout and other sessions keep being served.
- [x] #5 An unbounded response cannot exhaust memory: a stated size cap reports a readable error instead of the device running out.
<!-- AC:END -->

## Final Summary

<!-- SECTION:FINAL_SUMMARY:BEGIN -->
All three subtasks are done. libcurl 8.17.0 is a Vita portlib (TASK-017.01); the `curl` builtin and its engine in `src/sshd_curl.c` are in place (TASK-017.02); `wget` is a frontend over the same engine (TASK-017.03). Both are in the README with their flags.

Evidence, from the device runs recorded in the subtasks: `cd ~/Downloads && curl -LJO <release URL>` and `wget <release URL>` each leave `wifiturbo.suprx` at 51.6K (52879 bytes) with the release's SHA-1 (AC2). HTTPS verifies against `vs0:data/external/cert/CA_LIST.cer` by default, with `ux0:data/cacert.pem` as the fallback, and `-k`/`--no-check-certificate` is the only way off and prints a warning (AC3). A server that stalls after 10 bytes timed out at 30 s while a second connection still got its banner and a shell (AC4). An oversize Content-Length and a 256 MB undeclared stream both stopped with exit 63 and no partial file (AC5).
<!-- SECTION:FINAL_SUMMARY:END -->
