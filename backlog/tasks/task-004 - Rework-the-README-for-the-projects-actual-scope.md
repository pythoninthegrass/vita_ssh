---
id: TASK-004
title: Rework the README for the project's actual scope
status: Done
assignee: []
created_date: '2026-11-15T22:57:00Z'
updated_date: '2026-11-18T07:47:00Z'
labels: []
dependencies:
  - TASK-001.01
  - TASK-002.03
documentation:
  - README.md
  - AGENTS.md
  - CMakeLists.txt
modified_files:
  - README.md
type: docs
ordinal: 11000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
The README still describes the upstream project: the feature and limitation lists are client-only, and there is no server section telling anyone how to connect to a Vita from a devbox. Someone arriving at this repo cannot tell from the prose that the interesting half exists. This repo is standalone, not a fork, so the README should say what it is and keep the upstream credit where it belongs — a provenance note, not a title.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [x] #1 the README states what this repo is now (SSH client and SSH server apps for the Vita) and credits the upstream fork in a provenance note rather than as the title
- [x] #2 each app is described separately with its own titleid and VPK name
- [x] #3 the server section tells a user how to connect: what to install, where the password or key comes from, and one command line they can paste
- [x] #4 the feature and limitation lists match what the code does today, removing claims that are no longer true
- [x] #5 the prerequisites and build sections match the current task targets, including all portlibs the setup step builds
- [x] #6 no section claims functionality the server does not yet have; unfinished parts are labeled as such
<!-- AC:END -->

## Implementation Plan

<!-- SECTION:PLAN:BEGIN -->
Rewrite README.md in place, keeping the useful skeleton (screenshot, features, build, install, disclaimer) but re-cutting it around two apps instead of one client.

1. Split features and limitations per app, from the sources rather than from the old prose: client (password-only auth, exec channel + pty shell, 120x24 CSI-subset terminal, 1000-line scrollback, touch scrolling, up to 10 profiles in ux0:data/vitassh/profiles.dat); server (22 with 2222 fallback, 4 sessions, Ed25519 host key under ux0:data/vitasshd, authorized_keys or a generated bootstrap password, 21 builtins, per-session cwd, no pipes or redirection).
2. Move the fork and VitaShell credit out of the header into a Provenance section at the foot, worded as what the project stands on rather than as an apology for not being original. Keep the SonicMastr OpenSSL port and the SSH libraries credited too, since the build actually uses them.
3. Write the server's connect recipe: which VPK, what the console shows, where the password comes from, where authorized_keys goes, and commands that can be pasted. Say plainly that sftp/scp are refused today and that ssh-copy-id cannot work because the shell has no redirection.
4. Bring prerequisites and build in line with taskfile.yml and taskfiles/setup.yml: the three portlibs with their pinned versions, the single-target compile check, and the no-tests/no-CI caveat.
5. Replace the Contributing wishlist with the backlog as the queue, listing the client-side tasks filed for the wishlist items that are still open.

Risk: the server half is described from source, not from hardware. TASK-001.04 (smoke test) and TASK-002.03 (sftp/scp against real clients) will confirm or correct it, and the SFTP wording changes when TASK-002 lands.
<!-- SECTION:PLAN:END -->

## Implementation Notes

<!-- SECTION:NOTES:BEGIN -->
Stale claims removed, each checked against the code: "no scrollback" and "no touch input" (src/text.c scrollback, MAX_SCROLLBACK 1000, and the touch handling in APP_STATE_SIMPLE_SSH), "no interactive sessions: single commands only" (src/shell.c plus start_interactive_shell in src/ssh_client.c), the ASCII-only display line, and "libssh2 1.11.0+" which is pinned to 1.11.1 in libs/libssh2/VITABUILD alongside libssh 0.12.2.

Claims added from source, not guessed: the client never calls libssh2_knownhost_* so there is no host key verification; a single command's output is capped by execute_ssh_command's 16 KB accumulator; the garbled neofetch banner in the existing screenshot is PGF font coverage, so the README says so instead of hiding it; the server refuses the sftp subsystem in on_subsystem_request, so sftp and scp fail fast and ssh-copy-id has no redirection to work with.

The old Contributing wishlist became backlog tasks: TASK-005 with TASK-005.01 (client private key auth) and TASK-005.02 (host key verification), TASK-006 (command history), TASK-007 (full-screen programs), TASK-008 (touch menus), TASK-009 (SVG sources for the client's tile). Scrollback was on that wishlist too but is already implemented, so nothing was filed for it.

Tile claim corrected: assets/ holds SVGs for the server's tile only. sce_sys/*.png for the client arrived as PNGs with no source, which is what TASK-009 is for; the README now says which app is which instead of claiming both are rendered.

No LICENSE file exists in the repo. The README states that instead of inventing a license; the user should pick one.

Verification: no build was run (build/ belongs to the agent finishing TASK-001.02), so the check here is that every factual claim traces to a file and the markdown structure renders — tables, the two internal anchors, and the fenced blocks were read back after writing.

License gap closed while this task was open: LICENSE.md (Unlicense) landed in 452f56e and the README's License section (Unlicense for this repo's sources, LGPL-2.1 for the statically linked libssh, plus the third-party table) was written alongside this rewrite. The earlier note in this record saying the repo had no license file is superseded.

TASK-001.04 dependency added earlier to hold AC #6 open has been dropped on closure: the server section is labeled as unverified in the README itself, so no later task is blocked by this one. The SFTP paragraph still has to be revisited when TASK-002 lands.
<!-- SECTION:NOTES:END -->

## Final Summary

<!-- SECTION:FINAL_SUMMARY:BEGIN -->
README.md rewritten for the project's actual scope. The header's 'Prior art' blockquote is gone; the fork, VitaShell packaging conventions, the SonicMastr OpenSSL port and the SSH libraries are credited in a Provenance section at the foot, and the file opens by naming what the repo is now: two apps from one tree, ssh_client_vita (ROM000069, libssh2) and ssh_server_vita (ROM000070, libssh), each with its own section, tile and VPK name.

Feature and limitation lists were rebuilt from the sources rather than carried over. Stale claims removed: 'no scrollback', 'no touch input', 'no interactive sessions: single commands only', 'ASCII-only display', and the unversioned 'libssh2 1.11.0+' footer, replaced by the pinned versions in libs/*/VITABUILD (OpenSSL 1.1.1 Vita port, libssh2 1.11.1, libssh 0.12.2). Claims added only where the code backs them: password-only authentication with no host key verification in the client (libssh2_knownhost_* is never called), the 16 KB accumulator capping single-command output, PGF font coverage as the reason the screenshot's neofetch banner is garbled, and the server's 22/2222 fallback, 4 sessions, Ed25519 host key and authorized_keys under ux0:data/vitasshd, generated bootstrap password, 21 builtins, per-session cwd, and no pipes or redirection. The server section carries a status callout, refuses to claim SFTP (the subsystem is refused in sshd_session.c), and gives pasteable ssh commands including where the password and authorized_keys come from; ssh-copy-id is called out as unable to work while the shell has no redirection. Building matches taskfile.yml and taskfiles/setup.yml, including the single-target compile check and the no-tests/no-CI caveat.

The Contributing wishlist became backlog work instead of a paragraph: TASK-005 with subtasks TASK-005.01 (client private key auth) and TASK-005.02 (host key verification), TASK-006 (command history), TASK-007 (alternate screen and scroll regions), TASK-008 (touch selection in menus), TASK-009 (SVG sources for the client's tile, whose PNGs arrived with no source). Scrollback was on that wishlist and is already implemented, so nothing was filed.

Verification: no test suite exists and no build was run here, build/ belonging to TASK-001.02 while it was in flight. Both VPKs are present in build/ from that task's build, under the names the README states. Every factual claim in the file was traced to a specific source file before it was written, and the markdown was read back after writing to check the tables, fenced blocks and the internal anchors.

Follow-ups: when TASK-002 serves SFTP, the server section's 'refused today' wording and the ssh-copy-id note must change; TASK-001.04's smoke test should confirm the shell and sign-in descriptions against real hardware, since they are written from source rather than from a device.
<!-- SECTION:FINAL_SUMMARY:END -->

## Status Updates
<!-- STATUS:BEGIN -->
### Update: [2026-11-15 23:49:49 UTC]
- Retitled from "this fork's actual scope": the repo is no longer a fork, and a worker
  following the old title would write "this fork" prose straight into the README. The
  description's matching claim is corrected too.
- Do not redo the parts already handled while the repo was de-forked: the H1 is now
  "VitaSSH – SSH client and server for PS Vita", a provenance note under it credits
  rompelhd's VitaSSH and TheOfficialFloW's VitaShell, and the Generates section names both
  apps with their VPK names and titleids. That leaves #1 partly met (the opening paragraph
  still describes only the client) and #2 partly met (titleids appear, but there is no
  per-app description), which is why neither is checked.
- What is genuinely left: the client-only feature/limitation lists (#4), the how-to-connect
  section (#3), prerequisites/build accuracy (#5), and labeling unfinished server behavior
  (#6). #3 and #6 depend on TASK-002.03, so they cannot be settled before SFTP is verified.
<!-- STATUS:END -->
