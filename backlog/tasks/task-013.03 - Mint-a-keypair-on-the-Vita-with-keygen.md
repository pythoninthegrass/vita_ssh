---
id: TASK-013.03
title: Mint a keypair on the Vita with keygen
status: To Do
assignee: []
created_date: '2026-11-17 03:15'
labels: []
dependencies: []
documentation:
  - src/sshd_hostkey.c
  - src/sshd_builtins.c
  - README.md
parent_task_id: TASK-013
ordinal: 27000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
Everything in the key story assumes a keypair already made on another machine. The server already generates Ed25519 with ssh_pki_generate(SSH_KEYTYPE_ED25519, ...) for its host key, and libssh exports both halves (ssh_pki_export_privkey_file, ssh_pki_export_pubkey_base64), so it can hand over the 'ssh-ed25519 AAAA... comment' line to paste anywhere.

Honest scope note, and the reason this is the child to cut: a laptop's ssh-keygen is easier and this competes with it. It earns its place only where the Vita has to hold the private half, which is a second Vita's server or a host you reach from the Vita.

No passphrase: ssh_pki_export_privkey_file with a NULL passphrase stores the key unencrypted, because there is no terminal to type one into. The README has to say that plainly rather than let a user assume otherwise.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 keygen [FILE] writes an unencrypted Ed25519 private key and a .pub in 'algorithm base64 comment' form, and the server's own loader reads that .pub back as one authorized key
- [ ] #2 it prints the public line and the SHA256 fingerprint so what you pasted is checkable
- [ ] #3 it refuses to overwrite an existing private key or .pub without a flag, and says where it wrote
- [ ] #4 README states the private half is stored unencrypted and why
- [ ] #5 task build produces both VPKs
<!-- AC:END -->
