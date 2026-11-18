---
id: TASK-017.03
title: Add wget as a frontend over the curl engine
status: To Do
assignee: []
created_date: '2026-11-17 03:29'
updated_date: '2026-11-18T07:47:00Z'
labels:
  - shell
  - network
dependencies:
  - TASK-017.02
documentation:
  - AGENTS.md
modified_files:
  - src/sshd_builtins.c
  - README.md
parent_task_id: TASK-017
priority: low
ordinal: 42000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
wget is the other name people type first. It is a naming and defaults layer over the curl builtin's transfer code, not a second HTTP client -- a second client means two things to keep correct on a device that can only be tested on a device.

Depends on the curl builtin task for the shared engine: the redirect policy, certificate verification, size cap, timeout discipline and filename sanitization must be literally the same code path, so the two tools can never disagree about whether a download was safe.

wget's defaults are the ones worth matching: saves into the current directory under the URL's name without an explicit switch, -O FILE to name it, -q for quiet. -r (recursive) and -p (page requisites) are refused with a readable message: this is a fetcher, not a crawler.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 wget URL saves into the current directory under the URL's filename, matching curl -O for the same URL.
- [ ] #2 wget -O FILE URL writes that file, resolved through the path layer with the writability check, and a server-supplied Content-Disposition name can never override an explicit -O.
- [ ] #3 wget, curl -O and curl -J share one filename-sanitization path: a separator or .. in a server-supplied name is handled identically by both tools.
- [ ] #4 wget -q is silent and an error still reaches the terminal; a failing transfer exits nonzero.
- [ ] #5 -r and -p are refused with a readable unsupported-option message rather than ignored.
- [ ] #6 A failed transfer does not leave a partial file that looks like a completed download.
- [ ] #7 wget URL downloads the acceptance asset on a device with the correct name and size.
<!-- AC:END -->

## Definition of Done
<!-- DOD:BEGIN -->
- [ ] #1 task build produces both VPKs
- [ ] #2 Verified on a device with the same acceptance asset as curl
- [ ] #3 README lists wget side by side with curl and notes it is the same engine
<!-- DOD:END -->
