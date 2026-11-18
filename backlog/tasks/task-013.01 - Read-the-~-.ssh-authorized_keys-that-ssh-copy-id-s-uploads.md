---
id: TASK-013.01
title: Read the ~/.ssh/authorized_keys that ssh-copy-id -s uploads
status: To Do
assignee: []
created_date: '2026-11-17 03:15'
updated_date: '2026-11-18T07:47:00Z'
labels: []
dependencies:
  - TASK-014
documentation:
  - src/sshd_hostkey.c
  - src/sftp_server.c
  - scripts/smoke.sh
  - AGENTS.md
  - README.md
parent_task_id: TASK-013
ordinal: 25000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
ssh-copy-id's -s mode is four SFTP commands: '-get ~/.ssh/authorized_keys', '-mkdir ~/.ssh', 'chmod 700 ~/.ssh', 'put' the key file, 'chmod 600' it (/usr/bin/ssh-copy-id:316-342). Two things stop it.

load_authorized_keys() (src/sshd_hostkey.c:908) and stat_keys_identity() (:1133) each join a candidate data dir with the flat name authorized_keys, so a key uploaded into ~/.ssh/ is never read. And the two chmod lines carry no leading '-', so sftp -b aborts the batch on the SSH_FX_OP_UNSUPPORTED our SFTP layer answers for chmod, which AGENTS.md defends because a false OK makes the client believe it.

Agreed policy: chmod and chown answer OK only for .ssh and authorized_keys inside the server's own data directory, and keep OP_UNSUPPORTED everywhere else. That follows handle_perm_setstat(), which already honors a request that cannot change what the client observes.

Exec mode stays out of scope: it sends 'exec sh -c' with ';' '&&' '||' and '>>', and faking that means writing the shell AGENTS.md rules out. README should say so and point at -s.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 ssh-copy-id -s -i key.pub user@vita succeeds against a device whose only credential is the passphrase on the Vita's screen
- [ ] #2 the key it installs authenticates on the next ssh -i key login, with no server restart
- [ ] #3 ~/authorized_keys and ~/.ssh/authorized_keys are both honored when present, and the console names the file it read and the count; the reload identity stat shares the loader's candidate list so a changed file is still detected
- [ ] #4 chmod and chown answer OK only under .ssh/authorized_keys in the server's data dir; every other path still gets SSH_FX_OP_UNSUPPORTED, and the exception is written in AGENTS.md and README
- [ ] #5 a data directory that cannot be written reports that instead of reporting success
- [ ] #6 smoke.sh gains a copy-id check that fails when the key does not land, and task build produces both VPKs
- [ ] #7 Plain `ssh-copy-id` without -s (exec mode) is stated to remain unsupported, in README and in the task notes: its `sh -c` line needs a shell command, `;`, `&&`, `||` and `>>` handling, and TASK-025 chaining alone does not supply `sh -c`. README points at `-s` as the route that works and no longer says chaining alone would fix it.
<!-- AC:END -->
