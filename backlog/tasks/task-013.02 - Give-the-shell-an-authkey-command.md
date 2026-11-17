---
id: TASK-013.02
title: Give the shell an authkey command
status: To Do
assignee: []
created_date: '2026-11-17 03:15'
labels: []
dependencies: []
documentation:
  - src/sshd_builtins.c
  - src/sshd_hostkey.c
  - src/sshd_vpk.c
  - include/sshd.h
  - AGENTS.md
parent_task_id: TASK-013
ordinal: 26000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
One command per line, no redirection, no sh: the shortest honest route in is a builtin, because ssh vita@vita 'authkey add "ssh-ed25519 AAAA... laptop"' is a single command and needs none of the shell features this shell lacks.

libssh already has the pieces. ssh_pki_import_pubkey_base64() is the call parse_authorized_keys() uses (src/sshd_hostkey.c:814), so a key nobody can use is refused when you try to install it instead of being silently skipped at boot; ssh_get_publickey_hash() gives the SHA256 form the console already prints for the host key; MAX_AUTHORIZED_KEYS (32) and KEY_LINE_MAX (4096) are the limits the file already enforces and the command should too. Write through auth_state.keys_path, the file the loader actually read, not a fresh guess.

The hazard worth designing for: authorized_keys with one key kills the bootstrap password, so a mistyped or wrong key is a locked device with no keyboard. Follow the vpk builtin's shape (src/sshd_vpk.c) if this needs its own file, and remember a new .c goes in the server's add_executable list by hand.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 authkey add over an exec session installs a key that authenticates on the next connection, with no restart
- [ ] #2 an unparseable key, a duplicate of one already installed, a line over KEY_LINE_MAX and a 33rd key are each refused with a reason and leave the file unchanged
- [ ] #3 authkey list numbers the keys with type, bits, comment and SHA256 fingerprint, and authkey del N removes one so the next login reflects it
- [ ] #4 the write goes to the file the loader actually read, refuses a read-only device, and never leaves a truncated file
- [ ] #5 the install that takes authorized_keys from empty to one key prints the lockout warning and requires a confirmation flag
- [ ] #6 help lists authkey in the builtins[] table's alphabetical order, and task build produces both VPKs
<!-- AC:END -->
