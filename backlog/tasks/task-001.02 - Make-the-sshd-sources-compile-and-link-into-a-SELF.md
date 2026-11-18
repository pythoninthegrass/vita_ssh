---
id: TASK-001.02
title: Make the sshd sources compile and link into a SELF
status: Done
assignee: []
created_date: '2026-11-15T22:56:00Z'
updated_date: '2026-11-15T23:58:27Z'
labels: []
dependencies: []
documentation:
  - src/sshd.c
  - src/sshd_session.c
  - include/sshd.h
  - src/sshd_hostkey.c
  - AGENTS.md
modified_files:
  - src/sshd.c
  - src/sshd_session.c
  - include/sshd.h
  - src/sshd_hostkey.c
  - src/network.c
  - include/network.h
  - src/main.c
parent_task_id: TASK-001
priority: high
type: bug
ordinal: 3000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
The server sources were written against libssh 0.12.2's API from documentation and were never compiled. Three files (sh_path.c, sshd_builtins.c, vita_shims.c) already compile clean under arm-vita-eabi-gcc -Wall; sshd.c and sshd_session.c do not. Known breakage: sshd.c includes psp2/kernel/thread.h, which does not exist under that name in this SDK; sshd_session.c is missing the header that declares the server-callback structs and the channel-callback setter, so those types come back incomplete; sshd.h still declares a console-draw function that no longer exists, and its forward declaration of the bind type leaks into a parameter list; sshd_hostkey.c passes an unsigned pointer where libssh wants a signed one. Beyond those, expect the ordinary first-compile fallout in a file written untested. Constraints that must survive the fixes: output goes to the SSH channel and the vita2d overlay, never printf (there is no stdout and no sceDebugScreenInit in this SDK); the shell's cwd is per-session state, never the libc process-global one; the server is single-threaded, so callbacks must not block.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [x] #1 every sshd app source compiles with -Wall and no warnings that indicate a real defect
- [x] #2 the app links into a SELF and VPK with libssh's server symbols resolved
- [x] #3 no printf/puts/stdout use anywhere in the server sources; status text goes to the vita2d overlay and shell output goes to the channel
- [x] #4 no libc chdir() or getcwd() call anywhere in the server sources
- [x] #5 the server runs on its own kernel thread with an explicit stack size rather than in main()
- [x] #6 the poll loop takes exactly one accept per readable-listening-fd event
<!-- AC:END -->

## Status Updates
<!-- STATUS:BEGIN -->
### Update: [2026-11-15 23:58:27 UTC]
- Done. `task clean && task setup && task build` on a wiped tree produces both
  `ssh_client_vita.vpk` and `ssh_server_vita.vpk`; the second build reports "up to date".
- #1: the only warnings in the whole build are four pre-existing client-side ones
  (`text.c` unused `skip_next_prompt` / `last_processed_line` / `is_prompt_line`, and
  `main.c` `strncpy` bound equals destination). Zero from the seven server sources, with
  `-Wall` confirmed present in both targets' `flags.make`. Deliberately not fixed: those
  four are the client's and belong to no task here.
- #2: proven on the pre-link ELF, not the SELF — `nm` cannot read a SELF (it is an `SCE\0`
  signed container, its sections are encrypted, so `strings` finds no plaintext either).
  `ssh_bind_new`, `ssh_bind_accept_fd`, `ssh_set_server_callbacks`, `ssh_pki_generate_key`,
  `ssh_pki_ctx_new`, `ssh_event_add_fd` are all `T`; 192 libssh server-side `T` symbols in
  total; every one of the 46 `ssh_*` names the server sources call resolves except
  `ssh_callbacks_init` (a macro) and one mention inside a comment. `nm -u` shows only weak
  newlib/unwind symbols. The client and server are genuinely different binaries:
  `ssh_bind_accept_fd` is absent from the client, `libssh2_session_handshake` absent from
  the server.
- #3: needed a change outside the listed files. `src/network.c` is the one source compiled
  into both apps and had 14 `printf` calls. Rather than delete messages the client has
  always printed, it now calls `net_log()` (declared in `include/network.h`, with a
  `format(printf,1,2)` attribute) implemented per app: `sshd.c` forwards to
  `sh_console_log("%s", line)`, `main.c` forwards to `vprintf` and puts back the newline the
  call sites dropped, so the client's stdout is unchanged. The two remaining `printf`
  matches in the server sources are prose in comments.
- #6: `on_incoming` contains one `accept()`, no loop; verified by reading the function, not
  by counting lines in a grep window.
- Two fix choices worth keeping, since both look like cruft: `draw_console`'s first
  parameter is the real `vita2d_pgf *font` threaded from `server_thread` (the call site it
  replaced, `vita2d_get_current_params() ? NULL : NULL`, called a function this SDK does not
  have on a condition that could only pick NULL — the first pgf draw would have faulted), and
  `count_sessions` is `static`, hoisted above its callers, and reused by `draw_console` so
  the slot table is walked one way.
- libssh 0.12 API corrections recorded so the next port does not rediscover them: channel
  callbacks need `<libssh/callbacks.h>`; `on_channel_close`/`on_channel_eof` are
  `void`-returning; there is no `ssh_channel_set_callbacks_userdata` — set
  `channel_cbs.userdata` before `ssh_set_channel_callbacks`; the free for
  `ssh_get_publickey_hash` is `ssh_clean_pubkey_hash(&hash)`; `ssh_pki_generate` is
  deprecated in favor of `ssh_pki_ctx_new` + `ssh_pki_generate_key` + `ssh_pki_ctx_free`.
- Not in scope here, still open: the host key lives on `ux0:` so 001.03 must move it, and
  nothing has run on hardware yet — that is 001.04.
<!-- STATUS:END -->
