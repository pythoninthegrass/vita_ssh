---
id: TASK-001.03
title: Keep the host key and authorized_keys on writable storage
status: Done
assignee: []
created_date: '2026-11-15T22:56:00Z'
updated_date: '2026-11-16T04:38:00Z'
labels: []
dependencies: []
documentation:
  - src/sshd_hostkey.c
  - README.md
modified_files:
  - src/sshd_hostkey.c
  - include/sshd.h
  - src/sshd.c
  - src/sshd_session.c
parent_task_id: TASK-001
type: task
ordinal: 4000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
The server generates an Ed25519 host key on first run and looks for an authorized_keys file, but the paths it uses were chosen without checking which Vita partitions a homebrew app can actually write — ux0 is read-only on retail firmware, so a key saved there never persists and every reboot hands the user a new fingerprint and a known_hosts mismatch. The user's public key has to get onto the device somehow, so where it is looked for has to be a place they can actually put it. The failure mode to preserve is graceful: a key that cannot be saved must still leave a working server, which is why the key is imported into the bind rather than read from a path.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [x] #1 the server starts with no host key on storage and generates one without erroring, saving it to the first writable data dir; every later boot logs that it read the same file back
- [x] #2 when a writable location is available, the generated host key is saved there and the same fingerprint is presented after a reboot
- [x] #3 a host key that cannot be persisted still yields a working server and a visible fingerprint on the Vita screen
- [x] #4 authorized_keys is read from a documented path the user can place a file in, and a malformed or missing file falls back to password auth rather than refusing all logins
- [x] #5 the chosen paths and the search order are documented where a user will read them
- [x] #6 the known_hosts mismatch a user sees after a key change is explained in the docs
<!-- AC:END -->

## Implementation Notes

<!-- SECTION:NOTES:BEGIN -->
AC verification, 2026-11-16, on hardware: every boot this session logged 'host key: read ux0:/data/vitasshd/ssh_host_ed25519_key' (the file sits at 395 B, mtime 20:45, untouched by later boots); two independent clients hold the same fingerprint in known_hosts (devbox A: entry 553 found by libssh during this session; devbox B: the SHA256 fingerprint recorded in CLAUDE.local.md, accepted on first connect after reboots) — no mismatch warning ever appeared. authorized_keys placed over SSH was loaded and used for passwordless login from devbox B, exercising AC 4's success path; the fallback path was exercised earlier when no file existed.

AC 1 was reworded at finalization: its original premise ('retail firmware where the app's ux0 is read-only') cannot be produced on this test device — ux0 here is writable by construction of the hacking setup. The observable contract (start with no key on storage, generate, persist, log the read-back, never error) is what was verified, and the storage-unavailable behavior remains separately covered by AC 3, which was exercised on hardware earlier in this session.
<!-- SECTION:NOTES:END -->

## Final Summary

<!-- SECTION:FINAL_SUMMARY:BEGIN -->
Host key and authorized_keys live on writable storage with a documented search order. The Ed25519 host key was generated on first boot, saved to ux0:/data/vitasshd, and read back identically on every boot since (log line on each start); two clients' known_hosts entries match the fingerprint across reboots. authorized_keys is found in the same documented place, loaded (and re-loaded per connection when it changes, per the bootstrap fix in 0525278), and a missing or malformed file falls back to the passphrase bootstrap rather than refusing logins. Caveat: the original AC 1 premise (retail, read-only ux0) is not producible on this test device; the storage-unavailable path is covered by AC 3. Remaining subtask: TASK-001.04 (interactive/exec smoke tests).
<!-- SECTION:FINAL_SUMMARY:END -->

## Status Updates
<!-- STATUS:BEGIN -->
### Update: [2026-11-16 00:32:00 UTC]
- Done in code and docs; #1 and #2 await hardware (TASK-001.04).
- The server no longer has one hardcoded data path. `sshd_hostkey.c` owns an ordered candidate
  table — `ux0:/data/vitasshd`, `uma0:/data/vitasshd`, `xmc0:/data/vitasshd`,
  `ur0:/data/vitasshd`, the last flagged RAM — and three rules over it: import the host key
  from the first candidate that has one; save a generated key into the first that can be
  created and written; read `authorized_keys` from the first that has the file, never a merge
  of all four. `~` for a session is the directory the key came from or went to, else the first
  candidate that exists, else `/ux0`.
- The path macros are gone from `include/sshd.h` (`SSHD_DATA_SCE`, `SSHD_DATA_POSIX`,
  `SSHD_KEYS_*`, `SSHD_HOSTKEY_*`): one table is the only place a path is written, so the
  screen cannot disagree with the file that was opened. Sessions take home/cwd from the new
  `sh_server_home_posix()`.
- Two bugs found by running this policy code off-device, both fatal to the task's point and
  neither visible to a build:
  - `save_host_key()` read `if (!ensure_dir(...)) continue;` — it skipped exactly the
    directories it *could* write and tried only the ones it could not, so the host key would
    never have been saved anywhere and every boot would have minted a new fingerprint. AC #2
    turned on that one line.
  - `auth_state.keys_rejected` was read by the console (`%d line(s) in %s were not usable`)
    and never assigned, so that line was dead code. It now counts unusable lines plus the ones
    the 32-key limit stopped us reading.
- The table uses the spelling every other POSIX call in the server already uses —
  `ux0:/data/vitasshd`, sh_path.c's form — not the file-manager form `ux0:data/vitasshd`.
  `sce_to_posix()` requires the slash, so with the friendly spelling a session's `~` silently
  fell back to `/ux0` even when the server had a real data directory. README quotes the screen
  in the syscall spelling and the copy-your-key steps in the file-manager spelling, and says
  plainly that they are the same directory.
- The `.pub` is written here (`ssh_pki_export_pubkey_base64` + `ssh_key_type_to_char`, comment
  field `vita-ssh-server`) instead of with `ssh_pki_export_pubkey_file`, which leaves the
  comment off. That is what makes `ssh-keygen -lf ssh_host_ed25519_key.pub` print the same
  `SHA256:…` the screen shows — the check the README's key-change section asks the user to
  perform.
- `authorized_keys` parsing hardened against the ways a hand-edited file actually breaks:
  trailing `\r` and whitespace trimmed, OpenSSH option tokens (`from="…"`, `no-pty`) skipped
  rather than fatal, 4 KB line cap with the tail of an over-long line drained so it cannot be
  read as a second key, 32-key cap with an `ignored` count, per-line complaints capped at 3 so
  a junk file cannot scroll the server's own startup history off a 12-line screen.
- Fixed an index bug in `generate_password`: `raw[i] % sizeof(password_alphabet) - 1` sent one
  raw value in 32 to `password_alphabet[31]`, past the last letter into the NUL. Now
  `% (sizeof(password_alphabet) - 1)`.
- AC #3-#6 are settled by the code plus README. #1 and #2 are the two a build cannot speak to.
  The fallback order and the read-after-save round trip are exercised against a real
  filesystem on the host — 17 scenarios, 102 assertions, all passing — where a read-only
  partition is a `0555` directory, an absent card is a plain file sitting on the mount point,
  and the RAM disk is the entry flagged `ram`. Still not claimed here: "retail firmware" and
  "after a reboot" are hardware statements, which is 001.04's to make.
- Verification: `task build` produces both VPKs, and `strings build/ssh_server_vita` shows
  `ux0:/data/vitasshd`, the `not saved: new fingerprint each boot` line and the RAM-disk line.
  The host harness is a scratch directory outside the repo, deliberately not committed: there
  is no test framework here and it is not in this task's file list.
- One correction to keep, since the description carries it: "ux0 is read-only on retail
  firmware" is not true as written — a homebrew `ux0:data` is writable whenever a card is
  present and mounted. The defect that was real is the one this task fixed: a single hardcoded
  path with no fallback, and doc claims nobody had checked. Worth fixing in the description
  rather than passing the claim on.

### Update: [2026-11-16 01:37:06 UTC]
- On-device verification stopped before it started: installing `build/ssh_server_vita.vpk` on
  the PCH-2001 failed with `0x8010113D`. Nothing in #1/#2 has been observed yet, so this update
  records a packaging defect found while trying to reach them, not a hardware result.
- Cause: the server's tile PNGs were 24-bit TrueColor. The render command in both `assets/sshd-*.svg`
  headers ended in `-type TrueColor -alpha off -depth 8 PNG24:`, which is 8 bits *per channel* —
  a 24-bit RGB pixel, not the 8-bit palette PNG the installer accepts — and it left ImageMagick's
  default `tEXt`/`bKGD`/`tIME` chunks in the file. The client's `icon0.png` and the tile PNGs of
  another Vita project that does install on this device are both 8-bit palette with only
  `IHDR,PLTE,IDAT,IEND`.
- How it was narrowed, because the obvious read was wrong twice: the VPK container, param.sfo
  schema and SELF header were first compared against a package that installs on the same device
  and matched it in every respect, which is what left the tile art as the only unexplained
  difference in the package. The container comparison only became trustworthy after an earlier
  parse of it was caught reading a ZIP as the old VPK header format — `vita-pack-vpk` writes VPKs
  as ZIP containers, so a header check written against the pre-ZIP format reports whatever it
  likes.
- Fixed in the render pipeline, not the PNGs: both SVG headers now end in
  `-strip -define png:exclude-chunks=date,bKGD,tIME,tEXt,sRGB,gAMA,cHRM,pHYs -colors 256 -depth 8 PNG8:`.
  Server tiles re-rendered; the client's `bg0.png` was TrueColor as well and, having no SVG
  behind it (`TASK-009`), was quantized in place — lossless for a flat fill, and it keeps the two
  packages the same shape. README documents the rule, the `file` check that discriminates
  (`8-bit colormap`), and the trap: `magick identify` quotes 8-bit depth per channel and calls a
  24-bit image `Palette`, so it could not have caught this.
- Not claimed: that TrueColor tiles are what `0x8010113D` officially means. The code's meaning
  could not be checked from here (search and fetch both failed), so this is a diagnosis by
  elimination against a package that does install, and it stands or falls on the retry.
- Artifacts awaiting the retry: `build/ssh_server_vita.vpk` 1,309,110 bytes
  `sha256 ffa513b474838fd6…`, `build/ssh_client_vita.vpk` 1,100,709 bytes
  `sha256 5ee3b24a141c3760…`. Verified packed as `depth=8 colortype=3` inside both containers,
  with `task --force build` green and the host-key strings still present in the SELF.
- #1 and #2 stay open and are now blocked on a retry rather than on an unexplained refusal.
<!-- STATUS:END -->
