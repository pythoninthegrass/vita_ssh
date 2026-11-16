---
id: TASK-002.03
title: Verify sftp and scp against real clients on device
status: In Progress
assignee:
  - Claude
created_date: '2026-11-15T22:57:00Z'
updated_date: '2026-11-16T18:39:00Z'
labels: []
dependencies:
  - TASK-002.01
  - TASK-002.02
documentation:
  - src/sshd_session.c
  - README.md
modified_files:
  - README.md
parent_task_id: TASK-002
type: task
ordinal: 9000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
The SFTP path has one implementation and one set of assumptions behind it, and the client that actually exercises them is OpenSSH on a devbox. This is the evidence pass: byte-for-byte comparison both directions, the clients people really use, and the failures people actually hit. It also has to pin down the practical answer to 'how do I get my build onto the Vita', which is the reason the feature exists.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 scp of a file up and down matches sha256 in both directions, including a file over 10 MB
- [ ] #2 sftp recursive put and get reproduce a directory tree with the same file contents and sizes
- [ ] #3 operations on a read-only device and on a nonexistent path report a clear error to the client rather than an unhelpful status code
- [ ] #4 transfer throughput is recorded so a later regression is visible
- [ ] #5 the README documents a working copy-to-Vita and copy-from-Vita command line a user can paste
- [ ] #6 divergences between the macOS and Linux clients are recorded in the task notes
- [ ] #7 a multi-megabyte VPK put from OpenSSH lands byte-identical (cmp against the source) over WiFi in one command
- [ ] #8 legacy scp (-O) fails with a clear message instead of hanging
- [ ] #9 two concurrent sessions transfer at once without corrupting each other
<!-- AC:END -->

## Implementation Plan

<!-- SECTION:PLAN:BEGIN -->
Constraint: every criterion needs a live Vita running ssh_server_vita on the LAN. 192.168.8.57:2222 is unreachable from the authoring agent's sandbox (nc probe failed 2026-11-16), and smoke.sh already notes the agent's ssh cannot leave the machine. So the agent writes the harness and Lance runs it from a real terminal and pastes back the output; no criterion is checked from the agent's side without that output.

1. Extend scripts/smoke.sh sftp mode (the existing home of the sftp/scp checks) rather than adding a new script:
   - AC1: scp -s up and down of a 12 MB file, compared by sha256 (shasum -a 256 on macOS, sha256sum on Linux).
   - AC2: sftp -r put and get of a nested tree (several depths, empty file, 1-byte file, multi-block file); compare with a sorted `find` of path+size and per-file sha256.
   - AC3: capture client stderr for ops on /os0 (read-only) and a nonexistent path, for put, get, mkdir, rm, rename; assert the text names the cause, not just "Failure" or "Operation unsupported" where a clearer one is possible. Record the actual text in notes; a vague message becomes a follow-up (scope question for Lance), not a silent code change.
   - AC4: time the 12 MB and VPK transfers, print MB/s per direction, append a dated line to a throughput log in the task notes (not a repo file).
   - AC7: put build/ssh_server_vita.vpk (1.35 MB) to /ux0/data in one command, `cmp` against the source via a get-back.
   - AC8: `scp -O` with a timeout wrapper; assert non-zero exit within a few seconds and record the message.
   - AC9: two background scp/sftp transfers of different files at once, then sha256 both.
2. AC5: paste the README lines (README.md 'Copying files with sftp and scp') exactly as written against the device, fix any that fail.
3. AC6: run the whole sftp section from macOS (OpenSSH 10.6p1 here) and from a Linux client (Lance to supply a host or container on the same LAN); record divergences in notes.
4. Verification: `bash -n scripts/smoke.sh` and shellcheck locally; the device run is the real check.
5. Finalize per the task-finalization guide only with pasted output as evidence per criterion.

Open items for Lance: (a) is the Vita reachable and the server running, and will they run the script and paste output; (b) which Linux client to use for AC6.
<!-- SECTION:PLAN:END -->

## Implementation Notes

<!-- SECTION:NOTES:BEGIN -->
Evidence that already exists, recorded so it is not redone (none of the criteria below are checked, because this task's own pass has not been run):
- AC1: smoke.sh round-trips a 6 MB file over sftp and a small file over scp -s, byte-compared with cmp, not sha256, and nothing over 10 MB through scp.
- AC2: not covered; the smoke test uses single files and flat directories, there is no recursive put or get.
- AC3: partly; rename onto a directory and onto /os0 and a missing path return statuses, but the text the client shows has not been reviewed.
- AC4: not recorded; smoke.sh does not time transfers.
- AC5: README now has a 'Copying files with sftp and scp' section with sftp and scp -s commands; those exact lines were not pasted and run.
- AC6: not started; no Linux client has been used.
- AC7: a 64 MB put is killed mid-transfer in smoke.sh, but no completed multi-megabyte VPK put is compared against its source.
- AC8: not tried; scp -O should fail because the shell has no scp command, but the message and timing are unchecked.
- AC9: not tested; sessions overlap briefly in the logs, but no two transfers were run at once.
Dependencies TASK-002.01 and TASK-002.02 are Done, so this task can start.

2026-11-16: scripts/smoke.sh sftp mode now covers AC1, 2, 3, 4, 7, 8, 9 (new block 'TASK-002.03' in run_sftp). bash -n and shellcheck -S warning are clean for the new block. NOT RUN: the Vita is unreachable from the agent sandbox, so no criterion is checked. Next: Lance runs `scripts/smoke.sh sftp any@<vita-ip> 2222` from a LAN terminal (needs build/ssh_server_vita.vpk for the VPK check, perl for timing) and pastes the output, including THROUGHPUT lines and the 'op -> client text' lines for AC3. Still manual: AC5 (paste README commands), AC6 (Linux client run).

2026-11-16 device run (any@10.5.121.57:2222, macOS client OpenSSH_10.6p1, three smoke.sh sftp runs). Results: AC2 pass (tree names, sizes, sha256 identical). AC7 pass (1.35 MB VPK cmp-identical, 1.75-3.45 s). AC8: scp -O exits 1 in 0.57 s with 'scp: command not found', no hang. AC9 pass (two concurrent 8 MB scp uploads and downloads intact).

AC1 FAIL, reproducible: 12 MB scp -s download aborts with 'scp: Received message too long N' (N varied: 1447762625, 1269902808), i.e. the SFTP stream desynchronised mid-transfer. sftp get of the same size failed in 2 of 3 runs. The same 12 MB upload+download passed twice with identical bytes when run standalone (outside smoke.sh), so it depends on prior server state or timing. Uploads never failed. Not yet diagnosed; looks like a server-side defect in the download path (drain_out / queued replies), not a client quirk.

AC3 FAIL: writes to /os0 (put, mkdir, rm) reach the client as bare 'Failure' (e.g. `dest open "/os0/smoke.txt": Failure`). Missing paths are clear ('not found', 'No such file or directory'). Note the first run of the harness graded the /os0 cases as PASS because of a grader bug (pattern anchored at end of string); fixed.

AC4 baseline (WiFi, macOS): scp up 0.94-1.10 MB/s, sftp put 1.04-1.11 MB/s, sftp get 0.93-1.08 MB/s, scp down 0.48-0.91 MB/s (failed runs, not a valid baseline).

2026-11-16 AC1 diagnosis. The 'partial socket write is lost' hypothesis is wrong: libssh's channel_write_common queues packets in the session out buffer and returns the accepted count, which drain_out already honours. Current hypothesis: ssh_channel_write runs ssh_handle_packets_termination at its start and in its final ssh_channel_flush, which fires the channel data callback; sh_sftp_data() sees busy == 0 when drain_out() is called from run_requests' first line or from sh_sftp_pump(), so it runs run_requests() and drain_out() re-entrantly. The inner call advances or resets out_off/out_len (and may memmove the queue), the outer call then resumes with a stale pointer and offset: bytes sent twice or skipped, silent framing desync. Fits the random bad lengths, the timing dependence and the clean standalone runs. Instrumentation added in src/sftp_server.c (drain_out re-entry log, queue-moved-under-write detector, queued/accepted byte totals logged at session free). Needs a device run of the instrumented VPK before the fix.

smoke.sh AC3 block now runs all refused operations in one sftp session (- prefix, output parsed per echoed command).

2026-11-16 AC1 root cause CONFIRMED on device (instrumented build 11:07:35). During two concurrent 8 MB downloads: 'drain_out re-entered at depth 2' x4 (log capped at 4), then 'queue moved under ssh_channel_write: off 7360->98343 len 98343->207413 buf same'. The outer drain_out passed bytes 7360..98343 to ssh_channel_write; libssh's packet loop inside that call delivered the next request to sh_sftp_data() (busy == 0), whose run_requests/drain_out sent the same bytes and queued more; the outer call then advanced out_off by the stale count: 90,983 bytes sent twice and the next 90,983 never sent. Queued vs accepted totals still balance (duplicates and skips cancel), so only the moved-queue detector sees it. The client that desynced is the session whose reply total stopped short (7,408,704 vs 8,392,176).

Fix applied (not yet verified on device): drain_out() sets s->busy for its duration so a nested sh_sftp_data() only queues into s->buf; run_requests picks it up afterwards. The instrumentation stays in this build to show re-entry no longer occurs; strip it once the fix is verified.

Separate server bug found: an ssh exec command whose output exceeds 65,586 bytes is cut off (`cat sshd.log` of a 193,897-byte file returned exactly 65,586 bytes, then 'Connection closed by remote host'); `ls -l` of a directory printed one line the same way. Not part of this task's criteria; needs a decision (follow-up task?). Also the smoke.sh cleanup step 'rm -r transfer scratch' returned 1 in the failing run.

2026-11-16 exec output cutoff: exec output was cut to 65,536 bytes of stdout. Cause: after the command, sh_session_pump() sent exit status and EOF and returned 1, so the transport was torn down with output still queued in libssh. An interactive `cat` of the same 400,001-byte file delivered every byte, which ruled out dropped writes. Fix in src/sshd_session.c: close_wait() (the sftp graceful close, generalised) now also ends exec sessions: send status and EOF, close the channel, wait up to CLOSE_WAIT_S for the client's close. smoke.sh gained 'exec cat of the VPK is not cut off'. Not yet run on the device.

2026-11-16 device run with the drain_out fix (build 11:15:54): AC1 downloads now pass (12 MB scp -s and sftp get, and both concurrent downloads, sha256/cmp identical); instrumentation counters stayed at zero (no re-entry, no moved queue). The exec cutoff fix did NOT work: `cat` of the VPK over ssh exec is still cut at 65,536 bytes. close_wait() does not wait: ssh_channel_close() sets the channel state to CLOSED itself, so ssh_channel_is_closed() is true immediately (the same is true of the existing sftp wait, whose comment claims otherwise). ssh -vvv shows the client receiving exit-status, EOF and close before all data. Added logging in chan_write_raw/chan_write (return value, libssh error, window, finished flag) to find why the server stops writing; build 11:24 needs a device run.

2026-11-16 exec cutoff, device log (build 11:24): no 'write:' line fired, so every ssh_channel_write was accepted; the session was removed 65 ms after the exec line while most of the 1.35 MB was still queued in libssh. close_wait() now also waits while ssh_get_status() reports SSH_WRITE_PENDING (up to DRAIN_WAIT_S = 30 s), with the poll loop doing the sending; the two debug logs are removed. Build 11:37, not yet run on the device. Known limit, not addressed: exec `cat` of a huge file queues the whole output in libssh memory before any of it drains.

2026-11-16 exec cutoff, REAL cause (supersedes the three exec notes above, whose teardown/queued-output theory was wrong; the close_wait/DRAIN_WAIT changes were reverted): `ssh host cmd` with no stdin sends EOF right behind the exec request; on_channel_eof set s->finished, and cat's loop `read() > 0 && !sh_session_interrupted(s)` stopped after the first 64 KB chunk (SSHD_COPY_BUFSIZE), hence exactly 65,536 bytes. An interactive session keeps stdin open, which is why the same file came through whole. Fix: on_channel_eof ignores EOF for exec sessions (only sftp and shell sessions finish on it). Build 11:57, not yet run on the device. Also: one sftp get of 12 MB took 111 s (0.11 MB/s) with 12-17 s gaps between 32-request log markers; code identical to the 11:15 build that ran at 0.95 MB/s; cause not established, suspect network, needs a repeat run.

2026-11-16 device run, build 11:57 (server restarted 12:08:02, macOS OpenSSH_10.6p1): 77 pass, 3 fail, 1 skip. Exec cat of the VPK now returns all bytes (EOF fix confirmed). AC1: 12 MB scp -s and sftp get/put round trips and both concurrent downloads identical; instrumentation counters: no re-entry or moved-queue lines, queued == accepted in every session. Remaining failures: the three /os0 write messages (bare 'Failure'). Throughput is noisy over WiFi (scp-up 0.53, scp-down 0.25 with a 48 s run, sftp-put 0.66, sftp-get 0.93 MB/s), so no stable baseline yet. Instrumentation removed from src/sftp_server.c (build 12:13, only the busy-during-drain_out fix remains); AGENTS.md documents both rules.

2026-11-16 AC5, README commands pasted against the device (vita@10.5.121.57, OpenSSH 10.6 macOS, only host and user changed, no -i): `sftp -P 2222 vita@HOST` connects, pwd is /ux0/data/vitasshd, ls / lists the devices. `scp -s game.vpk vita@HOST:/ux0/data/` uploads byte-identical (re-downloaded and cmp'd). Fixed in README.md: the sshd.log download path was /ux0/data/sshd.log (no such file); it is /ux0/data/vitasshd/sshd.log (a relative `HOST:sshd.log` also works). The fsetstat sentence was wrong: every scp -s upload, including a 0644 file, prints 'scp: remote fsetstat: Operation unsupported' and exits 0 with the data intact, same for 0755; README now says so. The server logged no 'permission request refused' line for the 0644 uploads (only for the smoke test's chmod 600), so handle_perm_setstat() does not appear to be matching the FSETSTAT OpenSSH 10.6 sends (29-byte packet, type 10); not investigated. Also left on the Vita by an earlier test of mine and removed: /ux0/data/cutoff2.txt; (name includes the semicolon, from an unsupported `;` in an exec line).

2026-11-16 final-build device run (build 12:13, instrumentation removed; server started 12:36:20, macOS OpenSSH_10.6p1): 77 pass, 3 fail, 1 skip. The 3 failures are the /os0 message checks (TASK-002.03.01). Exec cat of the VPK complete; 12 MB scp/sftp round trips and both concurrent downloads identical. Throughput this run: scp-up 0.69, scp-down 0.93, sftp-put 0.64, sftp-get 1.01 MB/s (baseline procedure is TASK-002.03.03). This is the build to commit.
<!-- SECTION:NOTES:END -->
