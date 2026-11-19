---
id: TASK-017.03
title: Add wget as a frontend over the curl engine
status: Done
assignee:
  - claude
created_date: '2026-11-17 03:29'
updated_date: '2026-11-19T02:57:00Z'
labels:
  - shell
  - network
dependencies:
  - TASK-017.02
documentation:
  - AGENTS.md
modified_files:
  - src/sh_wget_args.c
  - include/sh_wget_args.h
  - scripts/test_sh_wget_args.c
  - scripts/test_sh_wget_args.py
  - src/sshd_builtins.c
  - src/sshd_curl.c
  - include/sshd_curl.h
  - CMakeLists.txt
  - README.md
  - AGENTS.md
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
- [x] #1 wget URL saves into the current directory under the URL's filename, matching curl -O for the same URL.
- [x] #2 wget -O FILE URL writes that file, resolved through the path layer with the writability check, and a server-supplied Content-Disposition name can never override an explicit -O.
- [x] #3 wget, curl -O and curl -J share one filename-sanitization path: a separator or .. in a server-supplied name is handled identically by both tools.
- [x] #4 wget -q is silent and an error still reaches the terminal; a failing transfer exits nonzero.
- [x] #5 -r and -p are refused with a readable unsupported-option message rather than ignored.
- [x] #6 A failed transfer does not leave a partial file that looks like a completed download.
- [x] #7 wget URL downloads the acceptance asset on a device with the correct name and size.
<!-- AC:END -->

## Definition of Done
<!-- DOD:BEGIN -->
- [x] #1 task build produces both VPKs
- [x] #2 Verified on a device with the same acceptance asset as curl
- [x] #3 README lists wget side by side with curl and notes it is the same engine
<!-- DOD:END -->

## Implementation Plan

<!-- SECTION:PLAN:BEGIN -->
1. Pure argv parser first (TDD, like sh_curl_name): src/sh_wget_args.c + include/sh_wget_args.h map wget's argv to a small result struct (url, -O file or `-O -`, -q, --no-check-certificate, --content-disposition) and refuse -r/-p/--recursive/--page-requisites and unknown options with a readable message. Host test scripts/test_sh_wget_args.py/.c under ASan/UBSan.
2. Engine: add `tool`, `insecure_flag` and `out_flag` to sshd_curl_opts_t so messages say `wget:` and name wget's own switches; the transfer code stays one path. curl sets the same fields to its own values.
3. cmd_wget in src/sshd_builtins.c (alphabetical row): defaults follow=1, fail_http=1, by_url_name=1 unless -O; -q is silent plus show_errors so errors still print; -O FILE wins over any Content-Disposition (engine choose_name already does this); `-O -` writes the body to the terminal. curl_resolve_out takes the tool name so its errors say wget.
4. CMakeLists server list gets sh_wget_args.c; task build must produce both VPKs.
5. Device run: vpk install -r, then wget of the wifiturbo.suprx asset into ~/Downloads (name, size, SHA-1), -O, -q with an error, -r/-p refusal, failed transfer leaves no file or .part.
6. README wget section beside curl; AGENTS.md host-check list, server source list, and the wget/curl shared-engine note.

Decisions: wget's own exit codes (4, 5, 8) are not mirrored; the engine's curl codes are returned. Existing target is overwritten, not renamed to NAME.1.
<!-- SECTION:PLAN:END -->

## Final Summary

<!-- SECTION:FINAL_SUMMARY:BEGIN -->
Added the `wget` builtin as a frontend over `sshd_curl_run()`: it sets follow and fail-on-HTTP-error, names the file by the URL unless `-O` gives one, and `-q` is silent plus show-errors. Options: `-q`, `-O FILE`/`-O -`, `--no-check-certificate`, `--content-disposition`; `-r`, `-p` and `-m` are refused with a sentence. The argv parser is pure (`src/sh_wget_args.c`, host test `scripts/test_sh_wget_args.py` under ASan/UBSan, shown to fail when the `-r` refusal is removed). The engine's messages now take `tool`, `insecure_flag` and `out_flag` from the caller so they say `wget:` and name wget's switches; `fetch_resolve_out()` serves both commands.

Device results (the Vita, home network, final build installed with `vpk install -r`): AC1 `wget URL` in ~/Downloads saved `wifiturbo.suprx`, 51.6K, same SHA-1 as a local download (also `-q -O NAME` and `-O -`). AC2 `-O` wrote its file, a nonexistent directory and `/os0` were refused (No such file or directory, Read-only file system), and `-O` beat a server Content-Disposition. AC3 hostile Content-Disposition values (`../../ux0/data/x`, `filename*=` with `%2F`) became bare names in the cwd under both `wget --content-disposition` and `curl -sJO`, nothing landed in /ux0/data, and `..` was refused. AC4 `-q` hides progress and a 404 still printed its error, exit 22. AC5 `-r` and `-p` print `not supported (this is a fetcher, not a crawler)`, exit 2. AC6 a stalled body timed out at 30 s and a 404 left no file or `.part`. AC7 as AC1.

Known differences from GNU wget, in the README: an existing file is replaced rather than kept as `NAME.1`, and exit codes are curl's (22, 28, 60, 63, 2), not wget's 1 to 8. Changes are uncommitted.
<!-- SECTION:FINAL_SUMMARY:END -->
