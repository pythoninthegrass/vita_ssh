---
id: TASK-012
title: Answer a new SSH connection instead of stalling before the banner
status: To Do
assignee: []
created_date: '2026-11-17 02:21'
updated_date: '2026-11-17 02:46'
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

Observed 2026-11-16 on the home Vita (192.168.8.57, port 22) after a run of connection attempts that never completed a handshake. ping answered at ~110 ms (device up, WiFi power-save), `nc -z` to port 22 succeeded, and `ssh -vv` printed its own 'Local version string SSH-2.0-OpenSSH_10.2' and then timed out waiting for the server's banner; 100 s of patience produced nothing. Restarting the server on the device (hold START, relaunch) cleared it.

Settled before you start, so the fix does not go the wrong way: the cap of 4 concurrent sessions (SSHD_MAX_SESSIONS, include/sshd.h) is a deliberate hardware limit, confirmed by Lance, not an oversight. Each slot is a libssh session with its own buffers and channel, all driven from one poll loop on a device with no swap. Nothing here should raise the cap. The defect to chase is that a socket which completes TCP and then never handshakes appears able to sit on one of the four slots -- if it does, silent sockets starve the accept loop, which matches what was seen: the kernel finishes the TCP handshake for the listen backlog while the app never services the new socket.

Treat that as the leading hypothesis, not the answer. The alternatives are a loop blocked inside a libssh callback or a session that was never freed after an abrupt client hangup, and AGENTS.md is explicit that both the one-accept-per-event rule and the nothing-may-block-in-a-callback rule are load-bearing.

Evidence to work from: ux0:data/vitasshd/sshd.log (readable over SSH, or from VitaShell in USB mode -- which this defect makes awkward, so plan for alternating between the two), the console lines the server prints on the Vita's screen for connections, logins and commands (it shows sessions N/4), and scripts/smoke.sh as the harness. Reproducing needs Apple's ssh: on the mini, /opt/homebrew/bin/ssh reports 'No route to host' for the Vita where /usr/bin/ssh connects (the scripts now prepend the system dirs to PATH, so running them is fine).
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 The cause of the pre-banner stall is identified and recorded in the task notes with evidence -- log lines, an instrumented build, or the accept loop and session table as observed at the moment a new connection is ignored -- rather than inferred from the symptom.
- [ ] #2 Silent sockets do not consume the cap: with 8 connections that complete TCP and send nothing, a real client still gets one of the four slots, and `ssh <vita> pwd` completes key exchange and returns 0. The cap itself stays at 4 -- SSHD_MAX_SESSIONS is a hardware limit, so raising it is out of scope.
- [ ] #3 A session that never authenticates is reclaimed within a stated timeout, and the reclaim shows up as a line in sshd.log naming the peer.
- [ ] #4 The reproducing sequence -- connect and stay silent, then connect properly -- passes on the device, and a fresh `scripts/smoke.sh exec` run afterwards is clean, including the cross-device `cd` and `mv` checks.
- [ ] #5 20 consecutive one-shot connections from a client that handshakes normally all complete, so a change that only clears the current wedge is not accepted.
- [ ] #6 Whatever rule the fix establishes (handshake timeout, session-slot accounting, accept-loop behaviour) is written into AGENTS.md in the same voice as the other server platform rules, and any limit that remains is in the README's server known issues.
<!-- AC:END -->

## Implementation Notes

<!-- SECTION:NOTES:BEGIN -->
2026-11-16: the wedge cleared when the server was restarted on the device, and immediately afterwards the same client ran scripts/smoke.sh exec 28 pass 0 fail and pty 16 pass 0 fail -- so the stall was server-side, not the radio and not the client. What preceded it: a run of connections that completed TCP and never finished the handshake (ssh attempts that timed out waiting for the banner, plus nc -z probes), each one a socket the accept loop had to deal with. Nothing was read from sshd.log or the Vita's console while it was wedged, so the cause is still unestablished and this task stays open. Reproducing it likely means inducing it the same way: open several sockets to port 22, send nothing, then try a real connection.

2026-11-16: Lance confirms the 4-session cap is a hardware constraint, so the goal is reclaiming a slot from a socket that never handshakes, never enlarging the table. The console's `sessions N/4` line is the cheapest witness for whether silent sockets are what occupy the slots -- read it on the device while the silent connections are open, before touching code.
<!-- SECTION:NOTES:END -->
