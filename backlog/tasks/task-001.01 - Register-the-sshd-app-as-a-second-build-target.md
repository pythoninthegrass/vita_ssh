---
id: TASK-001.01
title: Register the sshd app as a second build target
status: Done
assignee: []
created_date: '2026-11-15T22:56:00Z'
updated_date: '2026-11-15T23:58:27Z'
labels: []
dependencies:
  - TASK-001.02
documentation:
  - CMakeLists.txt
  - taskfile.yml
  - taskfiles/setup.yml
  - AGENTS.md
modified_files:
  - CMakeLists.txt
  - taskfile.yml
  - assets/sshd-icon0.svg
  - assets/sshd-bg0.svg
  - sce_sys_sshd/icon0.png
  - sce_sys_sshd/livearea/contents/bg0.png
  - sce_sys_sshd/livearea/contents/template.xml
  - README.md
  - AGENTS.md
parent_task_id: TASK-001
priority: high
type: task
ordinal: 2000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
Nothing in the build knows the server exists: CMakeLists.txt has one add_executable for the client and nothing globs, so the sshd sources are dead weight that no compiler ever sees. The server is a separate app with its own titleid because it ships as its own VPK alongside the client (the client keeps ROM000069). A worker needs to know that new .c files must be listed in add_executable by hand, that the Vita toolchain is arm-vita-eabi cross-compiled only (nothing here runs on the host), and that task — never bare cmake — is the entry point, since CMakeLists.txt reads VITASDK from the environment.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [x] #1 CMakeLists.txt builds a second app from the sshd sources with its own titleid, distinct from the client's ROM000069
- [x] #2 task build produces two VPKs and the client's VPK is unchanged in name and behaviour
- [x] #3 the server target links the static libssh from the SDK plus the libraries it needs (ssl, crypto, z, pthread) and the Vita stubs
- [x] #4 the server target defines the macro libssh's server-side declarations are gated behind, so its headers expose the server and SFTP API
- [x] #5 task build's fingerprint list covers the new sources and the libssh archive, so editing a server source or rebuilding libssh relinks the server VPK
- [x] #6 task clean and task setup still work, and the README build section names both artifacts
<!-- AC:END -->

## Status Updates
<!-- STATUS:BEGIN -->
### Update: [2026-11-15 23:39:28 UTC]
- Wiring done: second `add_executable`, `ROM000070` / `VitaSSHd`, server-only `WITH_SERVER`,
  a link list derived by measurement (`nm` + link probes, not copied from the client), and
  fingerprints covering the server sources and `libssh.a`. Two vita.cmake bugs fixed in
  CMakeLists.txt rather than worked around: flag accumulation across `vita_create_vpk` calls,
  and the missing VPK→SELF dependency edge that left a VPK packed once and never repacked.
- The server now ships its own tile rather than borrowing the client's: `sce_sys_sshd/` with
  icon0.png, bg0.png and template.xml, rendered from the SVG sources in `assets/`. Verified
  in a scratch tree with the TASK-001.02 fixes applied locally — both VPKs pack, each carries
  its own three art files byte-for-byte, and the client's SELF and param.sfo hash unchanged.
- AC #2 stays open on purpose: it asserts `task build` yields two VPKs, which cannot be true
  in-tree until TASK-001.02 makes the server compile. It has only been shown in the scratch
  tree. Everything else here is verified in this repo.
- Repo is no longer a fork: the upstream history was dropped for a single initial commit and
  `pythoninthegrass/vita_ssh` was created fresh, so `parent` is null. Prior art (rompelhd's
  VitaSSH, TheOfficialFloW's VitaShell) is credited in README.md and AGENTS.md.

### Update: [2026-11-15 23:58:27 UTC]
- AC #2 closed by TASK-001.02, which is Done: `task clean && task setup && task build` on a
  wiped tree yields `ssh_client_vita.vpk` and `ssh_server_vita.vpk`, and a repeat run reports
  "up to date". Task closed.
- Verified from inside the shipped VPKs, not from `build/`: each carries `sce_sys/param.sfo`,
  `eboot.bin` and its own three art files. The client's `param.sfo` hashes to
  `3e1113e5eeba07ea…`, byte-identical to the value recorded above, and reads
  `VitaSSH` / `ROM000069`; the server's reads `VitaSSHd` / `ROM000070`. `eboot.bin` sizes
  match the two `.self` files, so the SELF the VPKs ship is the one that just built.
- Worth knowing about "behaviour unchanged": AC #2 means the client's name and behaviour, and
  both hold — but the client's `eboot.bin` hash does move in 001.02, because `src/network.c`
  is compiled into both apps and its 14 `printf` calls became `net_log`. The client's
  implementation forwards to `vprintf` and re-adds the newline, so stdout is what it was. Name
  and behaviour are unchanged; the bytes are not, and no criterion claims they are.
- VPKs are ZIP containers (`PK\x03\x04`), so `unzip -l` / `unzip -p` is how to inspect what
  actually ships — `nm` and `strings` cannot read the SELF inside it.
<!-- STATUS:END -->
