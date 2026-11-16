---
id: TASK-002.03.03
title: Establish a repeatable SFTP throughput baseline and explain the stalls
status: Done
assignee:
  - claude
created_date: '2026-11-16T18:24:00Z'
updated_date: '2026-11-16T22:12:00Z'
labels: []
dependencies: []
references:
  - scripts/smoke.sh
  - src/sftp_server.c
documentation:
  - README.md
  - AGENTS.md
modified_files:
  - scripts/throughput.sh
  - scripts/transfer_lib.sh
  - scripts/smoke.sh
  - docs/throughput.md
  - docs/development.md
  - README.md
parent_task_id: TASK-002.03
priority: low
type: task
ordinal: 23000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
scripts/smoke.sh sftp prints THROUGHPUT lines, but WiFi runs vary too much to use as a regression baseline: across runs on 2026-11-16 a 12 MB scp upload ran 0.53 to 1.14 MB/s and a 12 MB scp download 0.25 to 1.03 MB/s, and one sftp get took 111 s (0.11 MB/s) with 12 to 17 s gaps between 32-request log markers where about 1 s is normal. The cause of the stalls was not established; the transfer code was identical to a build that ran at 0.95 MB/s. Without a stable number, a later slowdown cannot be told from WiFi noise, which is what TASK-002.03 acceptance criterion 4 is meant to prevent. Reading is also done in 32 KB requests, so whether the card or the SSH crypto limits throughput is unknown.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [x] #1 A documented repeatable procedure produces a throughput figure for each of scp up, scp down, sftp put and sftp get, as a median and range over several runs
- [x] #2 The conditions are recorded with the numbers: client, link, distance or channel, and the server build
- [x] #3 The cause of a stall such as the 111 s get is identified, or recorded as a network effect with the evidence
- [x] #4 Whether the memory card read speed or the SSH encryption limits throughput is stated with a measurement
- [x] #5 The recorded baseline and the procedure are written down where a later run can compare against them
- [x] #6 scripts/smoke.sh keeps printing parseable THROUGHPUT lines
<!-- AC:END -->

## Implementation Plan

<!-- SECTION:PLAN:BEGIN -->
1. New `scripts/throughput.sh user@host [port]` (kept out of smoke.sh so a baseline run is not 15 minutes of functional checks). It makes a 12 MB random file, runs N rounds (default 5, `RUNS=`) of scp up, scp down, sftp put, sftp get, prints one `THROUGHPUT` line per transfer in the exact format smoke.sh uses (AC6), then a `SUMMARY` line per direction with median, min, max in MB/s. Verifies sha256 on the last round so a fast wrong answer is not reported. Prints the conditions block (AC2): client version, local OS, link (Wi-Fi SSID/channel/RSSI via `airport`/`wdutil` when available, asked for otherwise), server build (git describe + VPK sha256, and the log's startup line).
2. Card vs crypto (AC4), measured in this order: (a) on-device `cp` of the 12 MB file within ux0 timed from the client via `ssh host "cp ..."`: card read+write with no network and no SSH crypto in the data path, so a floor on card speed; (b) exec `cat` of the file with `-c` set to aes128-ctr, aes256-ctr, chacha20-poly1305: if rates do not move with the cipher, crypto is not the limit; (c) compare both against the sftp/scp rates. State the conclusion with the numbers.
3. Stall cause (AC3): run the sftp get repeatedly while sampling, in parallel, a ping/latency probe to the Vita and the server log's READ markers. A stall that lines up with ping loss or latency spikes is recorded as a network effect with that evidence; a stall with clean ping while the server is idle points back at the server (window/pump) and gets a follow-up question rather than a silent code change.
4. Write the procedure and the recorded baseline (numbers plus conditions) into `docs/throughput.md`, link it from README and docs/development.md (AC5). Update smoke.sh header comment only if the THROUGHPUT format needs to be referenced.
5. No server source changes planned. Device runs need Lance's terminal (agent sandbox cannot reach the Vita); Lance runs the script and pastes the output, I fill in docs and the task record.
<!-- SECTION:PLAN:END -->

## Implementation Notes

<!-- SECTION:NOTES:BEGIN -->
2026-11-16 device runs from the Mac (wired en7) to any@10.5.121.57 on the Work network, VPK sha256 09edf6a8b78d, tree 5624021-dirty. bench RUNS=5: scp-up 0.67 (0.65-0.69), scp-down 0.65 (0.38-1.06), sftp-put 0.66 (0.65-0.66), sftp-get 0.99 (0.59-1.04) MB/s. limit RUNS=2: card cp 5.71 MB/s (read+write); exec cat aes-ctr 0.46, aes-gcm 0.66, chacha20 1.05 MB/s on a 0%-loss link. Full tables and conditions in docs/throughput.md.

AC3 left unchecked: the 83-94 s transfers of the shakedown coincided with ping loss 0.2-0.4% and rtt avg 57-133 ms (max 960 ms), and the same build then ran at 1.0 MB/s, so it reads as a network effect, but nothing shows it caused them. The baseline's moderate slow gets (0.59 MB/s twice, 0.46, 0.38) have no link signature and are unexplained, as is why uploads sit at 0.66 against a 1.05 download ceiling.
<!-- SECTION:NOTES:END -->

## Final Summary

<!-- SECTION:FINAL_SUMMARY:BEGIN -->
Added scripts/throughput.sh (bench and limit modes) and scripts/transfer_lib.sh (timed, rate, sha shared with smoke.sh, whose THROUGHPUT format is unchanged). Baseline and procedure are in docs/throughput.md, linked from README.md and docs/development.md. 2026-11-16, 12 MB, 5 rounds, VPK 09edf6a8b78d, wired Mac to Vita on 2.4 GHz ch 1 about 30 yd away: scp up 0.67, scp down 0.65 (0.38-1.06), sftp put 0.66, sftp get 0.99 (0.59-1.04) MB/s median. Card cp 5.7 MB/s, so crypto is the limit: exec cat 0.46 (aes-ctr), 0.66 (aes-gcm), 1.05 (chacha20) MB/s on a lossless link. The 83-111 s transfers are recorded as a network effect (0.2-0.4% loss, rtt up to 960 ms, same build later at 1.0 MB/s); correlation, not proof. Open and not tracked: the 0.59 MB/s gets with no link signature and the 0.66 MB/s upload ceiling below the 1.05 download ceiling.
<!-- SECTION:FINAL_SUMMARY:END -->
