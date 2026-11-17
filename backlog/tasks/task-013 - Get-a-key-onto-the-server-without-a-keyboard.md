---
id: TASK-013
title: Get a key onto the server without a keyboard
status: To Do
assignee: []
created_date: '2026-11-17 03:15'
updated_date: '2026-11-17T21:07:00Z'
labels: []
dependencies: []
documentation:
  - src/sshd_hostkey.c
  - src/sshd_builtins.c
  - src/sftp_server.c
  - README.md
  - AGENTS.md
ordinal: 24000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
The only credential you can create on the Vita is the three-word passphrase the server prints, and it exists to be replaced. Replacing it means a public key in ux0:data/vitasshd/authorized_keys, and today the only ways to get one there are VitaShell or a USB copy, which is why the README tells you ssh-copy-id will not work.

The expensive parts already exist: sh_auth_reload_if_changed() re-reads the file once per connection (src/sshd_hostkey.c:1247), so a key that lands is live at the next login with no restart, and ~ in both the shell and SFTP is that same data directory (its contents are authorized_keys, ssh_host_ed25519_key, ssh_host_ed25519_key.pub, sshd.log). What is missing is any route in over the network: ssh-copy-id -s gets as far as an SFTP put of ~/.ssh/authorized_keys, a path the server never reads, in a batch that two unprefixed chmod lines abort; exec mode sends 'exec sh -c' with ';' '&&' and '>>', which a one-command-per-line shell cannot run.

The three children are independent routes and any one of them removes the need to touch the card. Sequencing: 013.01 and 013.02 both answer 'which file do we read and write' in src/sshd_hostkey.c, so decide that once and let both follow it.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 a public key is installed into the file the server reads, from an SSH session, with no VitaShell, no USB and no restart
- [ ] #2 the console and sshd.log name the file keys were read from and how many, before and after an install
- [ ] #3 the route that takes authorized_keys from empty to one key warns that the bootstrap password dies at the next connection
- [ ] #4 the README's 'ssh-copy-id will not work yet' paragraph is replaced by what actually works, and AGENTS.md records the chmod exception 013.01 needs
- [ ] #5 task build produces both VPKs
<!-- AC:END -->

## Implementation Plan

<!-- SECTION:PLAN:BEGIN -->
Breakdown agreed with the user on 2026-11-16, all three approved for creation and none started.

- TASK-013.01 Read the ~/.ssh/authorized_keys that ssh-copy-id -s uploads — candidate-list change in sshd_hostkey.c shared by load_authorized_keys() and stat_keys_identity(), plus the narrow chmod/chown OK rule for .ssh and authorized_keys. Decided: OK only under the server's own data dir, OP_UNSUPPORTED elsewhere.
- TASK-013.02 Give the shell an authkey command — add/list/del, writing auth_state.keys_path, validating with the loader's own ssh_pki_import_pubkey_base64, lockout warning plus confirm flag on the first key.
- TASK-013.03 Mint a keypair on the Vita with keygen — weakest child, cut it if the other two need the time.

Sequencing: do 013.01 first because it settles which file is read and written; 013.02 then writes to the answer 013.01 picks. 013.03 is independent. Each child needs a device pass: copy-id, ssh -i login, and the console's file-and-count line are the evidence. No owner assigned yet.
<!-- SECTION:PLAN:END -->

## Implementation Notes

<!-- SECTION:NOTES:BEGIN -->
2026-11-17: status check, nothing here is done. What already works, and is the groundwork the description names: public-key login against ux0:data/vitasshd/authorized_keys. Seen on the work Vita from OpenSSH 10.6 with `ssh -v`: `Authentications that can continue: publickey`, `Server accepts key: ... id_rsa`, `Authenticated ... using "publickey"`, then the shell and `ls -la` showing authorized_keys (1.5K, dated Nov 15, so placed by hand, not by any route in this task), ssh_host_ed25519_key, ssh_host_ed25519_key.pub, sshd.log and sshd.log.old. The loader is rule 3 in src/sshd_hostkey.c and re-reads the file per connection.

Checked in the source at 3e461a8 and still missing: no `.ssh/authorized_keys` candidate in sshd_hostkey.c and no chmod/chown exception in the SFTP layer (013.01); no `authkey` row in builtins[] (013.02); no `keygen` (013.03). README line 201 still says ssh-copy-id will not work. No commit adds an install route; the related ones are ce00019 (searched, writable storage) and the backlog and README commits d6df4cb and 7d57331.

Note on sequencing: 013.01 depends on TASK-014, and the compound `sh -c` line ssh-copy-id sends in exec mode also needs TASK-025 chaining, which README line 201 mentions.

2026-11-17 correction to the sequencing note above: it said ssh-copy-id's exec-mode line "also needs TASK-025 chaining". That understates it. Per TASK-025's own description, chaining alone does not make plain ssh-copy-id work: the line is `sh -c '...'`, so the shell would also need an `sh` command, plus `>>` handling. Exec mode stays out of scope for 013.01 (its description already says so); only `ssh-copy-id -s` is covered, and 013.01 now has an AC (#7) making README state that.
<!-- SECTION:NOTES:END -->
