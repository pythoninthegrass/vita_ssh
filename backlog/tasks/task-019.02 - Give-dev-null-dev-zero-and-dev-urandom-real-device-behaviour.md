---
id: TASK-019.02
title: 'Give /dev/null, /dev/zero and /dev/urandom real device behavior'
status: To Do
assignee: []
created_date: '2026-11-17 03:38'
updated_date: '2026-11-18T07:47:00Z'
labels:
  - shell
  - path-layer
dependencies: []
documentation:
  - AGENTS.md
  - src/sftp_server.c
modified_files:
  - src/sh_path.c
  - include/sh_path.h
  - src/sftp_server.c
  - src/vita_shims.c
  - CMakeLists.txt
parent_task_id: TASK-019
priority: medium
ordinal: 45000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
Wanted: the three /dev entries that show up in shell one-liners, behaving like the real thing rather than merely opening.

/dev/null -- writes succeed and are discarded, returning the byte count as if they had landed; reads return EOF at once.
/dev/zero -- reads return the requested number of zero bytes; writes are discarded.
/dev/urandom -- reads return unpredictable bytes; /dev/random may be the same thing while nothing distinguishes them.

The seam already exists: __wrap_open() in src/sftp_server.c sees every open() in the server binary (builtins included, because active is NULL for them) and currently falls through to __real_open(). Recognize the synthetic names there, ahead of the SFTP branch, and serve them from a small table of synthetic file descriptors. Additional --wrap entries will be needed for the descriptors' read, write, close and fstat traffic; add each one to the link line in CMakeLists.txt alongside the existing list.

Three things that must not go wrong, each of them a rule already earned elsewhere in this tree:
- Synthetic descriptors are recognized by fd value, never by re-translating a path. The existing opendir bug is what happens when a path is translated twice.
- The synthetic fd values cannot collide with a real SceUID, and every wrapper forwards anything not in the table untouched.
- A wrapper that calls into libc must first establish that libc does not reach another wrapped symbol on that path.

Randomness is a correctness question, not a nicety: back urandom with the platform's random source. If it is unavailable, /dev/urandom fails with a readable error. Returning predictable bytes would be the most damaging lie this task could ship, because TASK-013.03 (mint a keypair on the Vita) would build keys on it.

SFTP has its own half: normalise_stat() flattens every entry to a plain file or directory, so the character-device mode has to survive ahead of it, and opening over sftp has to sink the same way.

Also make the shell's own redirect path treat /dev/null as a sink, so cmd > /dev/null creates no file -- the shell opens redirect targets through the path layer before the builtin runs, so check that route too.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 Writing to /dev/null returns success and the byte count and leaves no file on any device; reading it returns nothing immediately, including cat /dev/null exiting 0.
- [ ] #2 Reading /dev/zero yields the requested number of NUL bytes, bounded per read so a request for an enormous amount cannot stall the poll loop; writes to it are discarded.
- [ ] #3 Reading /dev/urandom returns different bytes on successive reads and is backed by the platform random source; if that source is unavailable the read fails with a readable error rather than returning predictable data.
- [ ] #4 stat, lstat and fstat report all three as character devices, and ls -l prints them with a leading c rather than a dash.
- [ ] #5 Over sftp, ls -l /dev reports character devices and a write to /dev/null is discarded: normalise_stat() does not flatten them into regular files.
- [ ] #6 unlink/rm, chmod, chown, rename and mkdir against any /dev path refuse with a readable error and a nonzero exit, from the shell and over sftp.
- [ ] #7 cd /dev/null reports Not a directory.
- [ ] #8 cat /dev/null | cat and cat /tmp/f | grep x both behave correctly once pipes exist, proving the synthetic descriptor flows through the same read path as a real one.
- [ ] #9 Every new wrapper forwards an unknown descriptor untouched to the real implementation, verified by normal file traffic still working after the wraps are added.
- [ ] #10 A file opened for read at the same time as a /dev descriptor keeps working: the synthetic table cannot shadow a real descriptor.
<!-- AC:END -->

## Definition of Done
<!-- DOD:BEGIN -->
- [ ] #1 task build produces both VPKs
- [ ] #2 Verified on a device over both an ssh session and an sftp session
- [ ] #3 CMakeLists.txt wrap list and the __wrap_ functions added stay in step, per AGENTS.md
<!-- DOD:END -->
