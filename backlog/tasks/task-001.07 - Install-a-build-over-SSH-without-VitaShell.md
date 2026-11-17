---
id: TASK-001.07
title: Give the shell a `vpk` command that installs builds over SSH
status: In Progress
assignee:
  - '@claude'
created_date: '2026-11-16T04:30:00Z'
updated_date: '2026-11-17T00:21:00Z'
labels:
  - deploy
  - server
  - shell
dependencies:
  - TASK-002
references:
  - TASK-002
documentation:
  - 'https://docs.alpinelinux.org/user-handbook/0.1a/Working/apk.html'
modified_files:
  - src/sshd_builtins.c
  - CMakeLists.txt
parent_task_id: TASK-001
priority: low
type: enhancement
ordinal: 20000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
The deploy loop today is: task build, USB mass storage, VitaShell drag, unmount, relaunch. The SSH server can already receive files once TASK-002 lands; what is missing is turning a VPK on disk into the running app's next launch.

The command is named `vpk` — apk-shaped (`vpk install FILE`, `vpk list FILE`) — rather than squatting on `install`, which coreutils owns with different semantics (copy plus mode bits) and which a future builtin should still be able to use. Nothing in this shell reserves command names: dispatch is a lookup of argv[0] in the builtin table and nothing else — there are not even POSIX reserved words here, since the shell has no grammar, only words.

The apparent chicken-and-egg — the shell asked to install the new build is inside the app being replaced — is not one: installing writes files, launching executes them, and the Vita loads the app from storage at launch, so the running instance can lay down the new binary and simply not be it. Vita homebrew already does this (VitaShell's updater installs its own new VPK over the running instance). The platform's patch overlay (ux0:patch/<TITLEID> loaded over ux0:app/<TITLEID> at launch) is the fallback if the running app's own files ever turn out to be locked: write the new build there and the next launch uses it, with deletion of the patch dir as the rollback.

`vpk` unpacks a VPK (a zip; zlib is already linked) into the app's own directory, refuses anything that is not this app's own title ID or whose eboot.bin is missing/implausible, and ends by telling the user to relaunch — the app cannot restart itself, and should not try. Verify the self-overwrite assumption and the patch fallback on hardware before committing to either.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 `vpk install /ux0/downloads/ssh_server_vita.vpk` unpacks the package and the app relaunches into the new build, verified on hardware by a version string or behavior change.
- [ ] #2 `vpk list FILE` prints a package's title ID and the files it carries, without installing anything.
- [ ] #3 `vpk` refuses a package whose title ID is not the server's own, and one with no plausible eboot.bin, each with a clear message.
- [ ] #4 Installing replaces only the files the package carries: a file already in the app directory that the package does not contain is left untouched (apk's overwrite rule, minus the .apk-new dance — the patch directory is this install's rollback instead).
- [ ] #5 An install never changes the data directory that holds the host key and authorized_keys (ux0:/data/vitasshd): credentials survive every install, verified on hardware. The package installs to ux0:app/<TITLEID>, so this holds by construction — the AC pins it against a future refactor that installs elsewhere.
- [ ] #6 A failed or interrupted install (client disconnect mid-unpack, full memory card) leaves the currently installed app launchable and unchanged — the old build still boots after deleting any partial artifacts.
- [ ] #7 After a successful install, quitting and relaunching does not resurrect the old build.
- [ ] #8 If the running app's directory turns out to be locked for overwrite, the patch-overlay route (ux0:patch/<TITLEID>) is implemented instead, with deleting the patch directory as the documented rollback to the installed base build.
- [ ] #9 `vpk` is listed in `help` and found by `which vpk`.
<!-- AC:END -->

## Implementation Plan

<!-- SECTION:PLAN:BEGIN -->
1. Zip reader, `src/vpk_zip.c` + `include/vpk_zip.h`: pure C plus zlib, no sce* calls, so it compiles on the host. Reads the central directory, lists entries, inflates (raw deflate via inflateInit2(-15)) or copies stored entries in bounded chunks to a sink callback, checks CRC32. Rejects zip64, encryption, and names that are absolute or contain `..`.
2. Host test first (TDD): `scripts/vpk_zip_test.c` + a Python generator for fixtures (stored, deflated, bad CRC, truncated, `../evil`). Run with the host cc against vpk_zip.c; nothing else in the tree builds on the host, so this is the only runnable check short of hardware.
3. `vpk` builtin in `src/sshd_builtins.c`: `vpk list FILE` (title ID from sce_sys/param.sfo, then entries, no writes) and `vpk install FILE`. Title ID is parsed from the package's param.sfo and must equal ROM000070; eboot.bin must exist and start with the SCE magic (`SCE\0`) and be of plausible size.
4. Install = stage then commit: unpack every entry into `ux0:data/vitasshd/vpk-stage/` (same device as ux0:app, so rename is cheap), verify CRCs, check free space first, then rename into `ux0:app/ROM000070/` with eboot.bin last. A failure before the commit phase deletes the stage dir and leaves the app untouched (AC6). Files the package does not carry are never touched (AC4); data dir is only ever used for staging under its own `vpk-stage` subdirectory, never for host key or authorized_keys (AC5).
5. Hardware questions that decide the route (AC1, AC8): (a) can this non-UNSAFE app write ux0:app/ROM000070 at all, (b) can it replace its own running eboot.bin. If either fails, switch the commit target to ux0:patch/ROM000070 (same code, different root) and document `rm -r` of that directory as the rollback.
6. Register in CMakeLists.txt (server list), add the builtin row in alphabetical order (after `uname`, before `which`), update README/docs.
7. Verify: host test, `task build`, then device run through ACs 1, 5, 6, 7, 8. Hardware ACs stay unchecked until Lance confirms.

Revised after the 2026-11-16 device run (Lance chose option 1): the commit root is ux0:patch/ROM000070, not ux0:app/ROM000070, because the app directory is read-only to the app even when UNSAFE. The install writes the whole package there, creating ux0:patch and ux0:patch/ROM000070 if absent; a failed commit removes a patch dir the install itself created. Rollback to the base build is `rm -r /ux0/patch/ROM000070`. To tell builds apart on the device, touch src/sftp_server.c before building: its startup line in sshd.log carries __DATE__ __TIME__. Open: whether the Vita launches from the overlay at all; first test installs a harmless build by VitaShell, then `vpk install` of a rebuilt one.

Revised again 2026-11-16 (Lance chose the promoter route and said head.bin may be vendored, since the Unlicense covers only his own files): `vpk install` unpacks and CRC-checks the package into ux0:data/pkg, writes sce_sys/package/head.bin (src/vpk_head.c fills the template in third_party/vitashell/head.bin, VitaShell GPL-3.0, vendored with its licence text and upstream commit), then calls scePromoterUtilityPromotePkgWithRif after loading the PAF and promoter modules, as VitaShell does. The server links ScePromoterUtil_stub. The commit/rollback and patch-overlay code is removed. README provenance and licence sections, AGENTS.md and third_party/vitashell/README.md updated. Host tests: scripts/test_vpk_zip.py and scripts/test_vpk_head.py (the latter compares against an independent Python transcription of makeHeadBin). `task build` is clean. Not yet run on a device: the running server must first be replaced through VitaShell because it still has the old install code.
<!-- SECTION:PLAN:END -->

## Implementation Notes

<!-- SECTION:NOTES:BEGIN -->
Implemented (uncommitted): src/vpk_zip.c + include/vpk_zip.h (zip reader), src/sshd_vpk.c (list/install), cmd_vpk row in sshd_builtins.c, SSHD_TITLEID compile definition from VITA_SERVER_TITLEID in CMakeLists.txt, AGENTS.md updated. Host test `python3 -I scripts/vpk_zip_test.py` passes (ASan/UBSan; stored, deflate, bad CRC, truncated, unsafe names, encrypted, bzip2, bad offset, sink abort). `task build` produces both VPKs with no warnings.

Design departure from the plan: commit moves each replaced file aside as <name>.vpkold and rolls every rename back on failure, rather than only deleting the stage dir, so the app is all-old or all-new even if a rename fails mid-commit. eboot.bin is committed first so a locked eboot fails with nothing to undo.

Not verified, needs a device: AC1, AC5, AC6, AC7, AC8. Open hardware questions: can this non-UNSAFE app rename/replace files under ux0:app/ROM000070, and can it replace its own running eboot.bin. If not, the commit root switches to ux0:patch/ROM000070 (AC8). Also unverified on device: sceIoRename onto a path in another directory on ux0, and the 0x3001 devctl free-space check on 'ux0:' (a failed devctl is treated as unknown and does not block).

Host test script renamed to scripts/test_vpk_zip.py (run: python3 -I scripts/test_vpk_zip.py); earlier note refers to it as vpk_zip_test.py.

Host test C file renamed to scripts/test_vpk_zip.c to match.

Hardware run 2026-11-16 (work network, port 22). Safe-mode build: ux0:app and ux0:patch invisible (ENOENT), df gives no device info. Switched the server SELF to UNSAFE (CMakeLists.txt); ux0:app/ROM000070 is then visible and df works. Side effect: the server now binds port 22 (SSHD_PORT_PRIMARY) instead of falling back to 2222; Lance chose to keep 22. smoke.sh, CLAUDE.local.md and README still say 2222.

Findings: `which vpk`, `help vpk`, `vpk list` (title ID + files) work on device (AC2, AC9 hold). Fixed on device: %zu printed 'zu' in sh_printf; remove_tree treated a missing dir as an error because opendir answers ENOTDIR for a missing path here. After the fix the install stages and CRC-checks, then fails at the commit with 'eboot.bin: cannot move the installed file aside: Permission denied', rolls back, removes the stage dir, and leaves the app and ux0:data/vitasshd untouched (partial evidence for AC6 and AC5).

AC8 trigger: ux0:app/ROM000070 is not writable by the app even when UNSAFE. touch, cp into it and mv of its eboot.bin all answer Permission denied; this is the directory, not a lock on the running binary. mkdir /ux0/patch succeeds (removed again). Whether ux0:patch/ROM000070 can be created and whether the Vita overlays it at launch is untested. In-place install (AC1 as written) is not possible on this device without a different mechanism.

Unrelated bug seen: SFTP upload over an existing file did not truncate (scp of a 1359770-byte file over a 1359776-byte one left 1359776 bytes); rm first, then upload, worked. Not part of this task.

Patch overlay result 2026-11-16: `vpk install` wrote the full package to ux0:patch/ROM000070 (5 files, base ux0:app copy and ux0:data/vitasshd untouched), the user quit and relaunched the app, and sshd.log still logged the old build stamp (sftp_server.c built 17:46:30; the patch eboot carries 17:50:07, confirmed from the local ELF). The Vita did not launch from the dropped-in overlay, so AC8's fallback does not work as a plain directory copy; it probably needs the patch registered with the system (promoter), not just files in place. Removed /ux0/patch/ROM000070 from the device afterwards. Candidate route: unpack to the stage dir and hand it to scePromoterUtil (what VitaShell's installer does), which needs SceLibPromoter and the PROMOTER_UTIL module; the app is already UNSAFE. Not started, needs Lance's decision.

The committed vpk install (stage, commit with .vpkold backups and rollback, patch root) is tested on device only as far as: stage + CRC, commit into a writable directory, patch dir creation, failure rollback against a read-only dir. It does not yet produce a new running build.

Promoter route research 2026-11-16 (read from VitaShell master package_installer.c, fetched with curl; Context7 /theofficialflow/vitashell only has build files, TinyFish fetch returned 401 until the user logged in): VitaShell's sequence is extract the VPK to ux0:data/pkg, write sce_sys/package/head.bin, load the PAF module (sceSysmoduleLoadModuleInternalWithArg, SCE_SYSMODULE_INTERNAL_PAF, argp {0x180000,-1,-1,1,-1,-1}), load SCE_SYSMODULE_INTERNAL_PROMOTER_UTIL, scePromoterUtilityInit, scePromoterUtilityPromotePkgWithRif(dir, 1), Exit, unload both. head.bin is a 1072-byte template (resources/head.bin) with the content ID written at 0x30 and three SHA-1-derived 16-byte MACs (fpkg_hmac) patched in at offsets read from the header. SDK has psp2/promoterutil.h and libScePromoterUtil_stub.a. BLOCKER for the decision: head.bin is VitaShell's GPLv3 resource file and this repo is Unlicense with a README statement that no third-party code is vendored; not copied. Awaiting Lance's choice.

Promoter run 2026-11-16 18:08: on the device `vpk install` unpacked and CRC-checked 5 files into ux0:data/pkg, wrote head.bin, then the system installer returned 0x80101009; ux0:data/pkg was removed and the installed app (eboot.bin 1362445 bytes, 18:06) and ux0:data/vitasshd were unchanged. The failing call was not named; added a stage name to the message (load PAF module, load promoter module, promoter init, promote package) and rebuilt. Hypotheses to test, not conclusions: the promoter refuses to replace an app that is running, or head.bin/param.sfo is rejected.

Promoter run 2 (18:10) with stage names: the system installer fails at "promote package" with 0x80101009; PAF and promoter modules load and init fine. Cause found in my code: the package's param.sfo has CONTENT_ID present but empty (length 1), sfo_string returned success, so the EP9000-<TITLEID>_00-0000000000000000 fallback never ran and head.bin carried a blank content ID. VitaShell falls back on an empty string. Fixed in package_open; rebuilt. Unconfirmed that this was the cause of 0x80101009; the other hypothesis (the system refuses to replace a running app) is still open.

Promoter run 3 (18:18) after the CONTENT_ID fix: the installer now fails at "promote package" with 0x80101114 instead of 0x80101009, so the empty CONTENT_ID was one real defect and the installer got further. 0x80101114 is documented only by community reports, not by the SDK headers: VitaShell issue #213 (https://github.com/TheOfficialFloW/VitaShell/issues/213, VitaShell's own VPK installed from inside VitaShell gives 0x80101114; the workaround given is to install it from a different app, molecularShell) and a reddit thread titled 'vitashell error 0x80101114' saying a VitaShell VPK cannot be installed from VitaShell itself. Reading: the promoter refuses to replace the app that is currently running. Not tested directly from this app (that needs a package for a different title ID, which package_check refuses). If right, a running server cannot install itself through the promoter, and neither the file-copy, patch-overlay nor promoter route lets the running app update itself; an install would have to be done by a different running app. AC1, AC4, AC6, AC7 and AC8 need rewriting or the task needs a different shape; waiting on Lance.

Running-app test 2026-11-16 18:2x (option 3): built a throwaway server variant (app ROM000070, package check hard-coded to ROM000071; not committed, CMakeLists.txt restored), plus a copy of the real VPK with param.sfo TITLE_ID patched to ROM000071 and the title text to VitaTest. Installed the variant by VitaShell, then `vpk install` of the test package from the running server: unpacked, handed to the promoter, returned success, and ROM000071 appeared in /ux0/app (eboot.bin and sce_sys present). So the pipeline (stage, head.bin with the CONTENT_ID fallback, PAF + promoter modules, PromotePkgWithRif) works from this app; the only difference from the failing runs is that the target app was not running. Conclusion: the promoter refuses to replace the running app (0x80101114), and a running server cannot install its own update. Side effect on the device: a VitaTest bubble (ROM000071) to be deleted from LiveArea by the user; my uploaded test VPKs were removed from /ux0/data. The normal build was rebuilt and uploaded as /ux0/data/ssh_server_vita.vpk (1360912 bytes). Side finding: the same mechanism lets the server install a different app's VPK, for example the client ROM000069, if the title check were widened.

CURRENT STATE as of 2026-11-16 (Lance paused to investigate further; nothing committed, status left In Progress, ACs and plan above are unchanged and partly superseded by the notes in order below).

Code in the working tree (uncommitted): src/vpk_zip.c + include/vpk_zip.h (zip reader), src/vpk_head.c + include/vpk_head.h (head.bin builder), src/sshd_vpk.c (`vpk list` and `vpk install` via the promoter), cmd_vpk in src/sshd_builtins.c, sh_vpk_* declarations in include/sshd.h, CMakeLists.txt (UNSAFE SELF, SSHD_TITLEID define, generated build/vpk_head_template.c, ScePromoterUtil_stub, new sources), third_party/vitashell/{head.bin,LICENSE,README.md} (VitaShell GPL-3.0, vendored with Lance's go-ahead), scripts/test_vpk_zip.{py,c} and scripts/test_vpk_head.{py,c}, README.md (provenance, licence, shell command list, vpk paragraph), AGENTS.md. Both host tests pass; `task build` is clean.

AC status by evidence: #2 and #9 verified on a device (`vpk list`, `help vpk`, `which vpk`). #3 partly: the title-ID refusal was exercised only indirectly (ROM000071 accepted by the throwaway variant, ROM000070 refused path not run); the eboot checks have not been run on a device. #5 holds so far: no run touched ux0:data/vitasshd except the earlier removed stage dir. #6 partly: stage-time and promoter failures left the installed app and ux0:data/pkg clean on the device; a client disconnect mid-unpack and a full card were not tried. #1, #4, #7, #8 are not met: the running server cannot install itself by any route tried (read-only ux0:app, ignored ux0:patch copy, promoter 0x80101114 on the running app).

What works on a device: the full stage + head.bin + promoter pipeline installs a package for a different, non-running title (ROM000071 test, confirmed in /ux0/app).

Open questions for the investigation: (1) is 0x80101114 really 'target app is running'; only community reports plus the ROM000071 control support it, no SDK or firmware documentation was found. (2) can the running app be made to not count as running, for example by installing with sync=0 and exiting, or by launching another app first; untested. (3) whether the server could instead update through a second app (client or a small helper) that promotes then relaunches the server; untested and larger than this task. (4) the narrower re-scope of installing the client (ROM000069) from the server needs only a wider title check; untested.

Device state to clean up: a VitaTest bubble (ROM000071) is installed and should be deleted from LiveArea; /ux0/data/ssh_server_vita.vpk holds the normal build (1360912 bytes) to reinstall through VitaShell, because the device was last known to be running the throwaway variant that accepts ROM000071. Work-network address and port: 10.5.121.57, port 22 (the UNSAFE build binds 22); smoke.sh, CLAUDE.local.md and README examples still say 2222.

Unrelated bugs seen, not part of this task: SFTP upload over an existing file does not truncate (rm first as a workaround); scp prints 'remote fsetstat: Operation unsupported' after every upload (documented in the README).
<!-- SECTION:NOTES:END -->
