---
id: TASK-001
title: SSH from a devbox into the Vita (sshd app)
status: In Progress
assignee: []
created_date: '2026-11-15T22:56:00Z'
updated_date: '2026-11-15T23:58:27Z'
labels: []
dependencies: []
references:
  - 'https://github.com/holzschu/a-shell'
  - 'https://github.com/theofficialflow/vitashell'
documentation:
  - src/sshd.c
  - src/sshd_session.c
  - src/sshd_builtins.c
  - src/sh_path.c
  - include/sshd.h
  - include/sh_path.h
  - AGENTS.md
modified_files:
  - src/sshd.c
  - src/sshd_session.c
  - src/sshd_builtins.c
  - src/sshd_hostkey.c
  - src/sh_path.c
  - src/vita_shims.c
  - include/sshd.h
  - include/sh_path.h
  - CMakeLists.txt
priority: high
type: feature
ordinal: 1000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
The repo's client app is the less useful half: typing on the Vita's on-screen keyboard is not how anyone wants to work. The value is the other direction — `ssh vita` from a macOS or Linux devbox, landing in a shell that exposes the whole filesystem, so files can be pushed, pulled, listed and inspected from a real terminal. This is a busybox-style shell: command functions run in-process (the Vita has no fork/exec), a shell-owned cwd per session, and POSIX paths mapped onto sceIo devices. The server sources exist in src/sshd*.c, src/sh_path.c and src/vita_shims.c but no build target knows about them and two of the files do not compile.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 ssh <user>@<vita-ip> from a macOS devbox authenticates and lands at a shell prompt
- [ ] #2 the shell reaches every mounted device through POSIX paths (ls /, ls /ux0:, cd between devices)
- [x] #3 task build produces the client VPK and the server VPK, and skips when nothing changed
- [ ] #4 with no authorized_keys installed, the server generates a password and shows it on the Vita screen
- [ ] #5 the client app is unaffected: same titleid, same behaviour as before this feature
<!-- AC:END -->

## Status Updates
<!-- STATUS:BEGIN -->
### Update: [2026-11-15 23:58:27 UTC]
- TASK-001.01 and TASK-001.02 are Done, so the app exists and builds: `task build` emits both
  VPKs and skips when nothing changed, which is all AC #3 asserts. Marked In Progress rather
  than Done because #1, #2 and #4 can only be settled on hardware — that is TASK-001.04, and
  it is now unblocked. #5 is separately verified, not assumed: the client's `param.sfo` inside
  its VPK is byte-identical to the pre-server build (`ROM000069`, title `VitaSSH`).
- Next open child is TASK-001.03 (host key and authorized_keys on writable storage). It is the
  right next step because none of it can be judged without hardware: the host key already
  targets `ux0:data/vitasshd` (writable) and its save is deliberately best-effort with a
  console warning, so the code path is plausible — but AC #1–#4 are claims about retail
  firmware and about a malformed `authorized_keys` falling back to password auth, and only a
  device can say whether they hold.
- TASK-001.03 is code-complete and its own ACs #3-#6 are checked. What it changed: the server
  owns an ordered list of data directories (`ux0:/data/vitasshd`, then `uma0:`, `xmc0:`,
  `ur0:` flagged RAM) instead of one hardcoded path, with three rules over it — import a host
  key from the first candidate that has one, save a new key to the first that can be created
  and written, read `authorized_keys` from the first that has the file and never merge them. A
  key that cannot be saved leaves a running server and says `not saved: new fingerprint each
  boot` on the screen. README now carries the table, the search order, and the known_hosts
  section.
- Read again against that update's last paragraph, which was too generous to the code it was
  describing: "its save is deliberately best-effort with a console warning" described
  behaviour that did not exist. `save_host_key()` tested `if (!ensure_dir(...)) continue;` —
  skipping precisely the directories it could write — so the host key was never saved
  anywhere, and the console's rejected-`authorized_keys`-lines counter was read but never
  assigned. Both compiled and linked without complaint. They surfaced only because the policy
  code was run off-device against a real filesystem. That is the argument for 001.04 being
  001.04: a green `task build` had already told us nothing about this file twice.
- #1, #2 and #4 therefore stay unchecked and belong to TASK-001.04, along with the two claims
  001.03 deliberately did not make for itself: that the in-memory fallback is what retail
  firmware does with a read-only card, and that a saved key presents the same fingerprint
  after a reboot. Suggested script for that run: follow README's "Where its files live" as the
  test plan — boot with no card, with a card, with the data directories present but
  unwritable, and compare the on-screen `SHA256:…` before and after a reboot, and against
  `ssh-keygen -lf` on the copied-out `.pub`.
<!-- STATUS:END -->
