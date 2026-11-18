---
id: TASK-012
title: Answer a new SSH connection instead of stalling before the banner
status: In Progress
assignee:
  - claude
created_date: '2026-11-17 02:21'
updated_date: '2026-11-18T07:47:00Z'
labels:
  - server
  - network
  - poll-loop
dependencies: []
references:
  - TASK-001.04
  - TASK-002.03
documentation:
  - src/sshd.c
  - src/sshd_session.c
  - AGENTS.md
  - scripts/smoke.sh
priority: high
ordinal: 19000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
The server accepts the TCP connection and then never sends its SSH version string, so the client dies with 'Connection timed out during banner exchange' and no session ever appears.

Observed 2026-11-16 on the home Vita (port 22) after a run of connection attempts that never completed a handshake. ping answered at ~110 ms (device up, WiFi power-save), `nc -z` to port 22 succeeded, and `ssh -vv` printed its own 'Local version string SSH-2.0-OpenSSH_10.2' and then timed out waiting for the server's banner; 100 s of patience produced nothing. Restarting the server on the device (hold START, relaunch) cleared it.

Settled before you start, so the fix does not go the wrong way: the cap of 4 concurrent sessions (SSHD_MAX_SESSIONS, include/sshd.h) is a deliberate hardware limit, confirmed by the user, not an oversight. Each slot is a libssh session with its own buffers and channel, all driven from one poll loop on a device with no swap. Nothing here should raise the cap. The defect to chase is that a socket which completes TCP and then never handshakes appears able to sit on one of the four slots -- if it does, silent sockets starve the accept loop, which matches what was seen: the kernel finishes the TCP handshake for the listen backlog while the app never services the new socket.

Treat that as the leading hypothesis, not the answer. The alternatives are a loop blocked inside a libssh callback or a session that was never freed after an abrupt client hangup, and AGENTS.md is explicit that both the one-accept-per-event rule and the nothing-may-block-in-a-callback rule are load-bearing.

Evidence to work from: ux0:data/vitasshd/sshd.log (readable over SSH, or from VitaShell in USB mode -- which this defect makes awkward, so plan for alternating between the two), the console lines the server prints on the Vita's screen for connections, logins and commands (it shows sessions N/4), and scripts/smoke.sh as the harness. Reproducing needs Apple's ssh: on devbox B, /opt/homebrew/bin/ssh reports 'No route to host' for the Vita where /usr/bin/ssh connects (the scripts now prepend the system dirs to PATH, so running them is fine).
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 The cause of the pre-banner stall is identified and recorded in the task notes with evidence -- log lines, an instrumented build, or the accept loop and session table as observed at the moment a new connection is ignored -- rather than inferred from the symptom.
- [x] #2 Silent sockets do not consume the cap: with 8 connections that complete TCP and send nothing, a real client still gets one of the four slots, and `ssh <vita> pwd` completes key exchange and returns 0. The cap itself stays at 4 -- SSHD_MAX_SESSIONS is a hardware limit, so raising it is out of scope.
- [x] #3 A session that never authenticates is reclaimed within a stated timeout, and the reclaim shows up as a line in sshd.log naming the peer.
- [x] #4 The reproducing sequence -- connect and stay silent, then connect properly -- passes on the device, and a fresh `scripts/smoke.sh exec` run afterwards is clean, including the cross-device `cd` and `mv` checks.
- [x] #5 20 consecutive one-shot connections from a client that handshakes normally all complete, so a change that only clears the current wedge is not accepted.
- [x] #6 Whatever rule the fix establishes (handshake timeout, session-slot accounting, accept-loop behavior) is written into AGENTS.md in the same voice as the other server platform rules, and any limit that remains is in the README's server known issues.
<!-- AC:END -->

## Implementation Plan

<!-- SECTION:PLAN:BEGIN -->
"1. Confirm on the device: build with the existing log lines, open one silent socket (`nc <vita> 22`, send nothing), then try `ssh <vita> pwd`; expect `peer X: starting key exchange` as the last line in sshd.log and no further console activity. Record in notes (AC #1).\n2. Make the handshake non-blocking and poll-driven: in on_incoming(), after ssh_bind_accept_fd(), ssh_set_blocking(ssh, 0), claim a slot immediately with a `handshaking` flag and a start time, add the session to poll_event, and call ssh_handle_key_exchange() once (it returns SSH_AGAIN while incomplete). Each server_loop pass retries ssh_handle_key_exchange() for handshaking slots until it succeeds or fails. No libssh call blocks.\n3. Reclaim: a slot that has not authenticated within HANDSHAKE_TIMEOUT_S (value to be settled with Lance) is freed with an sshd.log line naming the peer (AC #3).\n4. Silent sockets must not consume the cap (AC #2) without raising it: when the table is full and a new connection arrives, evict the oldest slot that is still pre-auth (handshaking or unauthenticated); refuse only if all four are authenticated. Option under consideration is a separate pre-auth pool instead, rejected for now because it adds libssh sessions beyond the hardware limit.\n5. Verify on a device: 8 silent sockets then `ssh <vita> pwd`; 20 consecutive one-shot connections; `scripts/smoke.sh exec` incl. cross-device cd and mv (AC #4, #5). Host tests cannot cover this (libssh and the poll loop do not build on the host), so TDD has no seam here; the device run is the failing-then-passing test, with a script under scripts/ for the silent-socket repro.\n6. Write the rule into AGENTS.md server platform rules and the remaining limit into README known issues (AC #6)."
<!-- SECTION:PLAN:END -->

## Implementation Notes

<!-- SECTION:NOTES:BEGIN -->
2026-11-16: the wedge cleared when the server was restarted on the device, and immediately afterwards the same client ran scripts/smoke.sh exec 28 pass 0 fail and pty 16 pass 0 fail -- so the stall was server-side, not the radio and not the client. What preceded it: a run of connections that completed TCP and never finished the handshake (ssh attempts that timed out waiting for the banner, plus nc -z probes), each one a socket the accept loop had to deal with. Nothing was read from sshd.log or the Vita's console while it was wedged, so the cause is still unestablished and this task stays open. Reproducing it likely means inducing it the same way: open several sockets to port 22, send nothing, then try a real connection.

2026-11-16: The user confirms the 4-session cap is a hardware constraint, so the goal is reclaiming a slot from a socket that never handshakes, never enlarging the table. The console's `sessions N/4` line is the cheapest witness for whether silent sockets are what occupy the slots -- read it on the device while the silent connections are open, before touching code.

2026-11-17 (source reading, not yet device-confirmed): the leading hypothesis (silent sockets sitting on slots) is wrong on the code. A slot is only claimed after `ssh_handle_key_exchange()` returns (src/sshd.c, end of `on_incoming()`, `slots[index].used = 1`), so a silent socket never holds one. The defect is the alternative the task lists: `on_incoming()` runs the whole handshake synchronously on a blocking session. libssh 0.12.2 `ssh_handle_key_exchange()` sends the server banner, then `ssh_handle_packets_termination(session, SSH_TIMEOUT_USER, ...)` (src/server.c:669); on a blocking session SSH_TIMEOUT_USER resolves to `opts.timeout`, and when that is 0 (nothing in src/ sets SSH_OPTIONS_TIMEOUT; options.c only copies or assigns it from the option) the wait is infinite (src/session.c:887-899). So the first connection that completes TCP and sends nothing parks the single poll thread forever: nothing is accepted, nothing is serviced, the already-open sessions freeze, and later connections complete TCP in the kernel backlog (`nc -z` succeeds) but get no banner because `ssh_send_banner` has not run for them. That matches every observation in the 2026-11-16 note, including that a restart cleared it. This is why the banner never arrived rather than arriving late. Still to do for AC #1: confirm on the device with an instrumented build (one silent socket, then read `peer X: starting key exchange` with no following line in sshd.log, and the console frozen).

2026-11-17: implemented in worktree branch worktree-task-012 (uncommitted), `task build` clean for both VPKs plus updater. Decisions from Lance: evict the oldest pre-auth session when all 4 slots are taken (cap stays 4, no extra memory), HANDSHAKE_TIMEOUT_S = 30. Changes: src/sshd.c (handshake now non-blocking and stepped from server_loop via `handshake_step()`; `slot_t` gained `handshaking` and `accepted_us`; `slot_oldest_preauth()` eviction in `on_incoming()`; reclaim line `reclaimed <peer>: no login within 30 s`; session set back to blocking in `handshake_done()` so later behavior is unchanged), `sh_session_authenticated()` in src/sshd_session.c and include/sshd.h, new scripts/silent_sockets.sh (8 silent sockets, `ssh pwd`, 20 one-shots). Not yet verified on a device, so no acceptance criterion is checked. Open risk to watch on the device: the session joins the poll event before key exchange finishes. Remaining: install the VPK on the Vita (needs Lance), reproduce the old stall first with the previous build if wanted for AC #1, run silent_sockets.sh and smoke.sh exec, then write the AGENTS.md rule and README known issue (AC #6).

2026-11-17 device run (work Vita, new build installed with `vpk install`, result.txt `promote package: 0x00000000`): scripts/silent_sockets.sh with 8 silent sockets open: `ssh pwd` returned /ux0/data/vitasshd, 20 of 20 one-shots rc 0 (21 pass 0 fail). sshd.log shows the silent sockets filling the 4 slots, then `evicting <peer> (no login after N s) for <peer>` as later ones and the real client arrived. A single silent socket held 38 s produced `reclaimed <peer>: no login within 30 s`. scripts/smoke.sh exec: 41 pass 0 fail, including `cd between devices` and both `mv across devices` checks. AC #1 is NOT checked: the cause is established from libssh source (blocking wait with opts.timeout 0 is infinite), and the old build was seen wedged on the same Vita (`Connection timed out during banner exchange` on ssh and scp, cleared by restarting the server), but the old build was not instrumented while wedged, so the log-line evidence the criterion asks for was not captured. The first run of silent_sockets.sh failed its 20 one-shots only because the script used `true`, which this shell lacks (status 127); it now uses `pwd`. The `Connection to <ip> closed by remote host.` line after exec output also appears on the old build, so it is not from this change.
<!-- SECTION:NOTES:END -->
