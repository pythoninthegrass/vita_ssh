---
id: TASK-017.02
title: Add curl with the flags needed to fetch a release asset
status: In Progress
assignee: []
created_date: '2026-11-17 03:29'
updated_date: '2026-11-19T00:04:00Z'
labels:
  - shell
  - network
dependencies:
  - TASK-017.01
  - TASK-014
references:
  - >-
    https://github.com/juanjsebgarcia/vita-turbo-wifi/releases/download/v0.7.2/wifiturbo.suprx
documentation:
  - AGENTS.md
  - include/sh_path.h
modified_files:
  - src/sshd_builtins.c
  - CMakeLists.txt
  - README.md
parent_task_id: TASK-017
priority: high
ordinal: 41000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
Wanted: cd ~/Downloads then

  curl -LJO https://github.com/juanjsebgarcia/vita-turbo-wifi/releases/download/v0.7.2/wifiturbo.suprx

leaves wifiturbo.suprx in ~/Downloads with the release's name and byte size. That is the acceptance test, run on a device.

Depends on the libcurl portlib task for the library, the resolver answer and the CA store, and on the home-directory task for ~/Downloads. Linking the server against libcurl means adding it to the server target in CMakeLists.txt alongside ssh, ssl, crypto, z and pthread -- and note the existing rule that SceAppMgr_stub and SceCommonDialog_stub are there for vita2d's sake and must stay.

Flags in scope: -L (follow redirects, bounded hop count), -O (name from the URL's last path segment), -J (name from Content-Disposition), -o FILE, -I (headers only), -f (fail on HTTP error status), -s (silent) and -S (show errors with -s), plus an explicit insecure switch for certificate verification.

Security and platform points that are not optional:
- A server-supplied filename is untrusted input. -J must strip any directory component and reject a name containing .. so a hostile host cannot write outside the target directory; -O gets the same treatment.
- Certificate verification is on by default against the CA store the portlib task settled; the insecure switch turns it off and says so on the terminal. A verification failure is a readable sentence, not a hex code.
- Nothing may block the poll loop: bounded connect/read timeouts, and the transfer reports progress in a way that keeps the Vita console and other sessions alive. The existing window-pacing and sh_server_poll_slice() rules are the precedent.
- Writes go through the path layer, and a read-only device answers Read-only file system the way mkdir does.
- A partial download is not left behind looking like a success: on failure, delete the partial file or say plainly what is there and why.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 curl URL writes the response body to the terminal; curl -o FILE URL writes it to FILE resolved through the path layer, and the parent directory's writability is checked before anything is created.
- [ ] #2 curl -O URL saves into the current directory under the URL's last path segment; curl -J uses the Content-Disposition name when the server offers one.
- [ ] #3 A server-supplied filename containing a path separator or .. is sanitized to a bare name or refused; a hostile Content-Disposition value cannot write outside the target directory.
- [ ] #4 curl -L follows redirects up to a stated hop limit and reports a redirect loop or an exceeded limit as a readable error.
- [ ] #5 The acceptance run on a device puts wifiturbo.suprx in ~/Downloads with the same size as the release asset.
- [ ] #6 HTTPS verifies the certificate by default; the insecure switch is the only way to disable it and prints a warning when used; an unverifiable certificate fails with a readable sentence naming the host.
- [ ] #7 curl -I prints the response headers without the body; curl -f exits nonzero on a 4xx or 5xx instead of saving the error page as if it were the file.
- [ ] #8 -s suppresses progress and -S restores error messages with it; without -s a progress indication updates as bytes arrive.
- [ ] #9 A stalled or very slow server hits a bounded timeout, reports it, and the poll loop keeps serving other sessions and the console.
- [ ] #10 A response beyond the stated size cap stops with a readable error naming the cap, and the partial file is removed or explicitly reported.
- [ ] #11 curl with no URL prints usage and exits nonzero; a malformed or unsupported URL reports which part was rejected.
<!-- AC:END -->

## Definition of Done
<!-- DOD:BEGIN -->
- [ ] #1 task build produces both VPKs
- [ ] #2 Verified on a device, including the wifiturbo.suprx acceptance run
- [ ] #3 README documents the supported flags and the CA store arrangement
<!-- DOD:END -->

## Implementation Plan

<!-- SECTION:PLAN:BEGIN -->
1. Host-testable pure helpers first (TDD, like sh_headtail): src/sh_curl_name.c with URL split/validation (scheme http/https only, host, rejected-part message), last-path-segment name for -O, and Content-Disposition filename extraction with directory stripping and rejection of '..'. Test under ASan/UBSan in scripts/test_sh_curl_name.py against a model, pinned on hostile inputs.
2. cmd_curl in src/sshd_builtins.c (alphabetical row in builtins[]): flags -L -O -J -o -I -f -s -S and an insecure switch; curl_multi driven from the poll loop with sh_server_poll_slice() and sh_session_yield() per step, bounded connect/low-speed timeouts, size cap, redirect limit (CURLOPT_MAXREDIRS), CAINFO vs0:data/external/cert/CA_LIST.cer then ux0:data/cacert.pem. Output via sh_write/sh_printf; file writes through resolve_arg/check_writable; partial file removed on failure.
3. Add new .c files to the server list in CMakeLists.txt; build both VPKs.
4. Device run: scp -s plus `vpk install -r`, then the wifiturbo.suprx acceptance run and each AC.
5. README flags and CA store section; add the test to AGENTS.md's host-check list.
<!-- SECTION:PLAN:END -->

## Implementation Notes

<!-- SECTION:NOTES:BEGIN -->
Partial downloads: when TASK-019.01 lands, curl's partial/temporary file resolves through the path layer to /tmp (the ur0: RAM disk) rather than sitting next to the final file, and the size cap has to account for the tmp bound measured in that task.

From TASK-017.01 device probe (work Vita): libcurl has no default CA path, so CURLOPT_CAINFO must always be set or every HTTPS request fails with verify error 20. Use vs0:data/external/cert/CA_LIST.cer (verified, HTTP 200) with ux0:data/cacert.pem as the fallback. getaddrinfo works as is. Link set and details are in AGENTS.md under 'libcurl portlib'.

2026-11-18 probe, no code kept: linking `curl` into the server (plus a reference to curl_easy_*; 97 curl_ symbols pulled) grows text from 2306476 to 3011840 bytes and the exe links, but `vita-elf-create` aborts: `Cannot allocate 3300 bytes for SCE data at end of segment 0; segment 1 overlaps`. readelf on the linked exe shows segment 0 ending at 0x812df538 and the RW segment starting at 0x812e0000, i.e. 2760 bytes of slack to the next 4 KB page against 3300 needed. So the AGENTS.md 'size ceiling' behaves as page-alignment slack after text, not a fixed byte limit: text can grow by 700 KB and fail or pass depending on where it lands mod 0x1000. Any link of libcurl needs a way to guarantee >= ~3.3 KB slack (or a larger gap) before it is worth building the command; decision needed before implementation.

2026-11-18: layout fix done (uncommitted). cmake/vita_seg0_slack.ld plus seg0_slack_anchor in src/vita_shims.c end segment 0 one byte past a page boundary, leaving 4095 bytes of slack for vita-elf-create regardless of text size. With `curl` in the server's target_link_libraries and a reference to curl_easy_*, `task build` produces all three VPKs (server VPK 1431300 bytes with the probe linked in). AGENTS.md size-ceiling bullet rewritten. The probe reference was reverted; `curl` stays in the link list but nothing references it yet, so the archive pulls nothing until the command lands.
<!-- SECTION:NOTES:END -->
