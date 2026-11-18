---
id: TASK-022
title: 'SFTP overwrite leaves stale trailing bytes: O_TRUNC never reaches __wrap_open'
status: Done
assignee: []
created_date: '2026-11-17 04:30'
updated_date: '2026-11-18T07:47:00Z'
labels:
  - sftp
  - data-corruption
dependencies: []
references:
  - >-
    https://datatracker.ietf.org/doc/html/draft-ietf-secsh-filexfer-02#section-6.3
documentation:
  - 'AGENTS.md (SFTP transport rules, the --wrap seams)'
  - 'src/sftp_server.c:204 (__wrap_open)'
  - 'src/sftp_server.c:279 (__wrap_close)'
  - scripts/test_vpk_zip.py
modified_files:
  - src/sftp_server.c
  - src/sftp_open_policy.h
  - scripts/test_sftp_truncate.c
  - scripts/test_sftp_truncate.py
priority: high
ordinal: 49000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
Overwriting an existing file with a shorter one through SFTP/scp silently leaves the tail of the old content in place. The client is told the transfer succeeded. This is silent data corruption on the write path, not a cosmetic issue.

Reproduced on a device today, deterministically, with nothing else running:

```
# t.big = 227 bytes (3 x 75-char lines), t.small = 6 bytes ("short\n")
scp t.big  vita:/ux0/data/vita-turbo-wifi/t.trunc.test   # -> 227 bytes, correct
scp t.small vita:/ux0/data/vita-turbo-wifi/t.trunc.test   # -> client exits 0
stat  /ux0/data/vita-turbo-wifi/t.trunc.test
  Size: 227            # expected 6
cat   /ux0/data/vita-turbo-wifi/t.trunc.test
short
AAAAAAAA...   # 75 A's - the previous file's bytes, still there
BBBBBBB...
CCCCCC...
```

Anything that rewrites a file in place and makes it smaller is affected: config files, dotfiles, `>` redirection, editors that write in place, and the `vpk install` staging path. It is invisible unless someone checks the size afterwards.

This was found while removing QuickMenuReborn from a device: pushing a 1373-byte taiHEN config over a 1504-byte one left a 1504-byte file whose tail was the overwritten original, including a fragment line `/vita-turbo-wifi)`. The workaround used was to remove the target first and then move a staged file into place; that is a workaround, not a fix, and it is easy to forget.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [x] #1 After `scp smaller_file vita:/path/to/existing_bigger_file`, the file on the device is exactly the size of smaller_file and its bytes are equal to smaller_file, verified by pulling it back and `cmp`-ing, not by trusting the client exit status.
- [x] #2 Overwriting with a file of exactly the same size, and overwriting with a larger file, both still produce byte-identical results.
- [x] #3 Overwriting a file inside a directory that requires path translation (a `/ux0/...` POSIX path) truncates correctly, so the fix cannot depend on bypassing the path layer.
- [x] #4 Overwriting on a read-only device still fails with the existing permission behavior rather than silently leaving the old content.
- [x] #5 A failed or partially-written overwrite does not leave a target file that mixes new head bytes with old tail bytes; the previous content either survives intact or is fully replaced.
- [x] #6 The mechanism is explained in the task notes: what flags the stock libssh handler actually passes to `open()`, and whether `SSH_FXF_TRUNC` survives translation. State the evidence, not a guess.
- [x] #7 If the conclusion is that `O_TRUNC` genuinely never arrives, say so in the notes and record whether the `.part` staging branch in `__wrap_open` has ever executed, since that branch is only reachable with O_CREAT|O_TRUNC.
- [x] #8 A host-runnable check under scripts/ covers a shrink-overwrite, a grow-overwrite and a same-size overwrite against the real write path or an extracted equivalent, built with the host cc like scripts/test_vpk_zip.py.
- [x] #9 Behavior confirmed on a device for all three size relationships, with `stat` output transcribed into the task notes.
<!-- AC:END -->

## Definition of Done
<!-- DOD:BEGIN -->
- [x] #1 `task build` produces both ssh_client_vita.vpk and ssh_server_vita.vpk.
- [x] #2 The shrink-overwrite repro in the description is re-run on a device and now yields the correct size and bytes.
<!-- DOD:END -->

## Implementation Plan

<!-- SECTION:PLAN:BEGIN -->
## Verified today (device run, do not re-derive)

- Repro is deterministic: 227-byte file, then scp a 6-byte file over the same path. Client exits 0. `stat` reports **Size: 227** and `cat` prints `short` followed by the previous file's three 75-char lines.
- No `.part` file was left behind, and none was present during the failure.
- Real-world trigger: pushing a 1373-byte config over a 1504-byte one produced a 1504-byte file whose last 131 bytes were the overwritten original, including the orphan line `/vita-turbo-wifi)`. `cmp` caught it only because the pulled-back copy was compared against the intended bytes. Any verification that trusts the scp exit status misses this entirely.

## Where to look

`src/sftp_server.c:239` guards the whole truncating design:

```c
if ((flags & O_CREAT) && (flags & O_TRUNC)) {   /* stage to <target>.part */
    ...
    fd = __real_open(part, flags, mode);
} else {
    fd = __real_open(t.sce, flags, mode);        /* <-- shrink lands here */
}
```

`__wrap_close` (line 279) is correct: it renames the part file over the target, and because sceIoRename will not replace an existing file it handles `EEXIST` by unlinking the target and renaming again. Given that logic is sound and no `.part` file ever appears, the strong inference is that **the `else` branch is what runs**, i.e. `O_TRUNC` is not set in the flags the stock libssh handler passes. Confirm that first, before changing anything.

So the branch that is supposed to handle truncation may be unreachable in practice. That is the thing to establish, and it reframes the fix: this is not a bug in the rename, it is that truncation is never requested or never survives translation.

## Establish the mechanism before fixing

1. Log the raw `flags` inside `__wrap_open` for a create, a shrink-overwrite, and a grow-overwrite, and read them from `sshd.log`. Use `sh_console_log`, never printf. Note that the build stamp only changes with `task --force build`, so check the `sftp: server code built` line to be sure which binary you are reading.
2. Compare against what libssh 0.12.2 actually maps `SSH_FXF_TRUNC` to. The stock handler is `sftp_server_handlers.c`; the library is under `.task/portlibs` after `task setup`, and the installed headers are in `$VITASDK`. If the client sets `SSH_FXF_TRUNC` and the handler drops it, the fix belongs at the wrapper. If the client never sets it, no amount of server-side flag handling fixes it and the server must truncate based on the SFTP open pflags itself.
3. Check whether newlib's `open` on this platform honors `O_TRUNC` at all when it reaches `sceIo*`. If it does not, a flag-only fix cannot work and the server must unlink or truncate explicitly - note that `__wrap_truncate` already exists at line 482 and goes through `sceIoChstat`, and that the SFTP `truncate` path already reads the size back and answers `ENOSYS` when it does not change. Reuse that verified approach rather than inventing a second one.

## Constraints that will bite

- Nothing may block inside a libssh callback. Whatever the fix is, it must not add a blocking call to the open path - see the SFTP transport rules in AGENTS.md.
- `--wrap` rewrites calls made inside libc too. If the fix calls into another libc function that is wrapped (for example `unlink` or `rename`), it will be re-entered through the wrapper with `active` still set, and the path will be translated a second time. `__wrap_opendir` already has to clear `active` around its real call for exactly this reason - follow that precedent.
- The staged-write design exists for a reason: a partially-written file that mixes new and old bytes is worse than a failed write. Do not fix truncation by opening the target directly with O_TRUNC, or a half-written upload now corrupts the original instead of leaving it alone. Keep write-to-temp-then-promote.
- `errno` is what clients see. A truncation failure should surface as a real error, not a success.

## Checks

- Fast compile: `cmake --build build --target ssh_server_vita`. Packaging: `task --force build`.
- Device: run all three size relationships (shrink, grow, same size) and pull each back for `cmp`. `scp` to this server prints `remote fsetstat: Operation unsupported`, which is harmless; omit `-p`.
- The host check cannot reach sceIo, so factor the decision (given open flags and whether the target exists, what does the server do) into a pure function and drive that, mirroring the shape of `scripts/test_vpk_zip.py`.
<!-- SECTION:PLAN:END -->

## Implementation Notes

<!-- SECTION:NOTES:BEGIN -->
Mechanism established on the home Vita (UNSAFE build, port 22, devbox B, instrumented build installed with `vpk install`; the staged VPK was cmp-verified first).

Evidence:
- scp from OpenSSH 10.2 opens with pflags 0x0a (WRITE|CREAT, no TRUNC): `scp -vvv` prints `Sent dest message SSH2_FXP_OPEN ... M:0x000a`. `sshd.log` shows `sftp: open /ux0/data/vita-turbo-wifi/t.trunc.test flags=0x201 creat=1 trunc=0 wr=1 rdwr=0` for every upload, including the shrink.
- The follow-up request is type 10 (FSETSTAT), 29 bytes: handle plus flags plus permissions only. It carries no size, so nothing else tells the server to truncate.
- libssh 0.12.2 `process_open` (src/sftpserver.c:1114-1163) maps SSH_FXF_TRUNC (0x10) to O_TRUNC correctly and parses the flags correctly (src/sftpserver.c:226-238). The constants in include/libssh/sftp.h match the draft.
- A client that does send TRUNC works: paramiko `open(p, 'w')` logged `flags=0x601 creat=1 trunc=1`, took the `.part` staging path, and shrank 225 to 6 bytes correctly.

Conclusions for AC #6 and #7: SSH_FXF_TRUNC survives translation when the client sends it. O_TRUNC does not arrive from OpenSSH 10.2 scp because the client never asks for it. The `.part` branch in `__wrap_open` is reachable and has executed (paramiko run), but never for scp. The original inference that the else branch runs because O_TRUNC is dropped was right about the branch and wrong about the cause.

Shrink repro on device with the instrumented build: first=225 OK, shrink want=6 got=225 MISMATCH, same-after-shrink OK, regrow OK, same OK, grow want=450 got=450 OK (cmp on pulled-back copies).

Fix (option 1): `sftp_open_stages()` in src/sftp_open_policy.h stages every CREAT open that is not APPEND or EXCL, and not a read-write open without TRUNC, through `<target>.part`. `__wrap_open` calls it instead of testing O_CREAT|O_TRUNC, and opens the part file with O_TRUNC so a leftover .part from a failed upload cannot leak its tail. The reasoning for staging on CREAT alone is the notes above: scp never sends TRUNC.

Host check: `python3 -I scripts/test_sftp_truncate.py` passes (policy table plus shrink, same, grow and shrink-with-TRUNC against files on the host).

Device run on the home Vita, build stamp 2026-11-17 01:46:37, all pulled back and cmp-ed: first 225 OK, shrink want=6 got=6 OK, same size 225 OK, grow 450 OK, shrink again 6 OK. `ls -l` showed t.trunc.test at 6B and no .part file left behind.

AC #4 verified on the home Vita (server built 2026-11-17 01:52:46, which contains the staging fix): `scp small` and `scp big` over the existing read-only file /os0/psp2config_dolce.skprx both failed with `dest open ...: Permission denied` and scp exit 1. The file was pulled back before and after and is byte-identical (5266 bytes, cmp equal), and no .part file appeared. An earlier attempt hung, but the sshd.log shows that connection died in the handshake (`handshake failed ... Socket error: Success`, 01:54:04) while another agent had the server's connection slots saturated, so it never reached an SFTP open; it says nothing about the read-only path.
<!-- SECTION:NOTES:END -->
