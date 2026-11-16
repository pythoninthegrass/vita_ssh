---
id: TASK-006
title: Recall previously typed commands in the client
status: In Progress
assignee: []
created_date: '2026-11-16T00:00:00Z'
labels: []
dependencies: []
documentation:
  - src/shell.c
  - src/main.c
  - src/menu.c
  - include/sshd.h
type: task
ordinal: 13000
modified_files:
  - README.md
  - include/input.h
  - include/menu.h
  - include/text.h
  - src/input.c
  - src/main.c
  - src/menu.c
  - src/text.c
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
The client keeps no record of what you typed. Single-command mode opens the IME on an empty field every time, so retyping the same path on a small on-screen keyboard is the daily cost of using the client at all, and the profiles file proves the app can already persist per-user state to ux0:data. The interactive pty path is different: shell.c forwards the shoulder buttons as raw arrow escapes, so history there belongs to the remote shell and should stay that way. The sshd app already keeps a 12-line per-session history (SSHD_HISTORY in include/sshd.h), which is the precedent for how much to keep.
<!-- SECTION:DESCRIPTION:END -->

## Implementation Notes

`task build` is green from a wiped `build/` and `.task/` on the committed tree, run again after the
commits landed: both `build/ssh_client_vita.vpk` and `build/ssh_server_vita.vpk` are produced, and
`strings` on the client ELF confirms the history path and format strings are linked in. Only AC #6 is
checked, because it is the only criterion the host can verify. There is no emulator and no host test
suite in this repo, so AC #1 to #5 are implemented and reviewed against the criterion but left
unchecked.

Where each criterion lives:

- **#1 needs on-device verification.** `APP_STATE_SIMPLE_SSH` in `src/main.c` handles
  <kbd>↑</kbd>/<kbd>↓</kbd> through `history_select()` in `src/menu.c` and echoes the
  selection with `terminal_print_history()` in `src/text.c`. <kbd>○</kbd> and <kbd>✕</kbd> both
  prefill the IME via `param.initialText` from `cmd_history.entries[selection]`, widened to UTF-16
  by the new `utf8_to_utf16()` in `src/input.c`. `selection` starts at `count`, so the first prompt
  after a launch or an accepted command offers the newest entry. With an empty history
  `history_select()` returns NULL, the field is prefilled empty, and the d-pad is a no-op — same as
  before.

  Button mapping, and the one place this departs from the old file: the spec says <kbd>✕</kbd> opens
  the prefilled IME, but <kbd>✕</kbd> was Connect. That is not a lost binding — the terminal screen
  is only entered from the profiles list, which already calls `connect_ssh()`, and <kbd>START</kbd>
  both disconnects and leaves the screen, so `ssh_connected` is always 1 while this screen is up and
  <kbd>✕</kbd> was dead. <kbd>✕</kbd> now opens the prompt as specified, and the `connect_ssh()`
  fallback is kept on it for the disconnected case only. <kbd>○</kbd> keeps working exactly as
  before, so a habit of reaching for <kbd>○</kbd> is not broken.
- **#2 needs on-device verification.** `history_load()`/`history_save()` in `src/menu.c` use the
  `profiles.dat` pattern (`sceIoOpen`/`sceIoRead`/`sceIoWrite`, fixed-size records) against
  `ux0:data/vitassh/history.dat`, called from `main()` at launch and from `history_add()` on every
  accepted command. `MAX_HISTORY` is 32 in `include/menu.h`, matching `SSHD_HISTORY` in
  `include/sshd.h` without including that header. Entries are `MAX_COMMAND_LENGTH` (256) bytes, which
  is the cap the README already states for a single command.
- **#3 needs on-device verification.** `history_add()` returns early on NULL and on
  `command_is_blank()` (spaces, tabs, CR, LF), and on a repeat it shifts the older copy out and
  re-appends rather than adding a second record. `history_save()` skips blanks again, so blanks can
  never reach the file even from a corrupt one on disk.
- **#4 needs on-device verification.** There is exactly one `history_add()` call site, in the
  single-command branch of `APP_STATE_SIMPLE_SSH`. The four credential fields in
  `get_ssh_credentials_from_profile()` (including `kb(u"Password:", …, 1)`) feed only
  `SshProfile` and `save_profiles()`, which writes `profiles.dat`, never `history.dat`. The shell's
  `Shell Command:` and `Select Special Key:` prompts never call into history either.
- **#5 needs on-device verification.** `src/shell.c` is untouched. The new d-pad handling sits in
  the `else` arm of `if (shell_is_active())`, so while the pty is up the shoulders still go to
  `shell_handle_controls()` -> `shell_send_special_key("\x1B[A"/"\x1B[B")` and nothing local consumes
  the d-pad.

Logic-level checks that were run on the host (not a substitute for the device): the four history
functions were compiled natively against a stubbed `sceIo*` layer and driven through 22 assertions —
newest-first walk, no wraparound at either end, blank/NULL rejection, move-to-newest on repeat,
eviction past 32, save/reload fidelity, over-long command clamped to 255 characters, a torn trailing
record truncating the load, and the empty-history no-ops. All passed. The block under test is
extracted verbatim from `src/menu.c` rather than retyped, so the assertions cover the shipped code.
They do not exercise the IME, the render, or real `ux0:` behaviour.

Two notes for whoever picks this up. `save_profiles()`/`load_profiles()` and the new
`history_save()` all pass `0777` to `sceIoOpen`: that is the mode-literal convention already used in
16 places across `src/`, and the hex form (`0x1ff`) would obscure what the value means. And the lens
diagnostics in this environment report `psp2/ctrl.h`, `vita2d.h` and `include/text.h` as missing and
`AppState`, `SshProfile` and `ip` as undeclared. Those are not real: every one of those headers
exists, `ip` is declared at `include/ssh_client.h:6`, and `arm-vita-eabi-gcc -fsyntax-only
-Iinclude` compiles all five changed sources with exit 0. The host clangd has no Vita sysroot, so it
cannot see the SDK. Trust `task build` and the cross-compiler here.

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 single-command mode offers the previous command in the IME field and walks newest-first through earlier ones
- [ ] #2 history survives disconnecting and relaunching the app, and is bounded to a fixed number of entries
- [ ] #3 repeating a command and submitting a blank line do not add duplicate or empty entries
- [ ] #4 passwords entered in the credential fields are never written to the history file
- [ ] #5 the interactive shell is unchanged: arrows still reach the remote shell untouched
- [x] #6 task build produces both VPKs
<!-- AC:END -->
