---
id: TASK-001.07
title: Give the shell a `vpk` command that installs builds over SSH
status: Done
assignee:
  - '@openai'
created_date: '2026-11-16T04:30:00Z'
updated_date: '2026-11-17 01:36'
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
  - CMakeLists.txt
  - include/sshd.h
  - include/vpk_update.h
  - src/sshd_session.c
  - src/sshd_vpk.c
  - src/sshd_updater.c
  - src/vpk_promote.c
  - scripts/test_vpk_updater.py
  - README.md
  - AGENTS.md
parent_task_id: TASK-001
priority: low
ordinal: 20000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
Provide `vpk list FILE` and `vpk install FILE` so a server build can be uploaded and installed over SSH without repeating the VitaShell deploy loop. Only the server's own title ROM000070 is accepted.

Hardware established that ux0:app is read-only to the app, a plain ux0:patch copy is ignored at launch, and the system promoter refuses the currently running title (0x80101114). The final implementation therefore stages and validates the package in ux0:data/vitasshd-update/server, installs the bundled updater ROM000072, closes the SSH command, launches the updater and immediately exits. The updater promotes the server, records its result, and attempts to relaunch the server even on failure.

System promotion uses whole-package replacement: omitted app-directory files may be removed, as verified with a probe file. Persistent files belong in ux0:data/vitasshd, which is never used for staging. The app guarantees staging validation/cleanup, not rollback of system-installer failures. A one-time VitaShell bootstrap is required for older server builds; retain a recovery VPK. the user approved these revised semantics.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [x] #1 `vpk install FILE` stages a server VPK and hands off to the bundled updater, which installs it and relaunches the server into a distinguishable build, verified on hardware.
- [x] #2 `vpk list FILE` prints the package title ID and files without installing anything.
- [x] #3 `vpk` refuses another title ID and a missing, undersized or invalid-magic eboot.bin with clear messages.
- [x] #4 Installation uses system promotion with whole-package replacement semantics; files omitted from the new VPK may be removed. Persistent user files belong in the data directory, and this behavior is documented.
- [x] #5 Host key and authorized_keys in ux0:data/vitasshd survive installation, verified on hardware by unchanged identity and credential checksums.
- [x] #6 Failures before updater handoff leave the installed server unchanged and clean partial staging where possible. System-installer failure has no app-managed rollback guarantee; the updater records the result and attempts to relaunch the server even on failure, and VitaShell recovery is documented.
- [x] #7 After a successful update, a separate quit and relaunch still runs the updated executable, verified on hardware.
- [x] #8 A bundled separate updater title performs promotion after the server exits; no direct ux0:app overwrite or plain ux0:patch overlay is used.
- [x] #9 `vpk` is listed in help and found by which vpk.
<!-- AC:END -->

## Implementation Plan

<!-- SECTION:PLAN:BEGIN -->
Final approved and executed plan:
1. Retain existing ZIP reader, package inspection and head.bin builder; enforce own-title and plausible eboot checks.
2. Factor the PAF/promoter module lifecycle into src/vpk_promote.c; share fixed staging paths/title definitions through include/vpk_update.h.
3. Build a minimal UNSAFE updater title ROM000072 and bundle updater.vpk in the server VPK, with explicit SELF/VPK dependency edges.
4. Stage and CRC-check server/helper packages away from credentials and VitaShell's stage. Promote the trusted bundled helper, write a one-shot ready marker, drain command output/status/EOF/close, launch helper and immediately exit.
5. Helper consumes the marker, targets only ROM000070 for shutdown if needed, promotes the server, writes result.txt, and relaunches the server even on promotion failure. Document system-operation limits and whole-package replacement.
6. Verify host ZIP/head/updater tests and task build; bootstrap once via VitaShell, exercise distinct-build update, rejection/staging failure, repeated helper promotion, credential checksums, omitted-file behavior, and manual quit/relaunch persistence on the home Vita.
<!-- SECTION:PLAN:END -->

## Implementation Notes

<!-- SECTION:NOTES:BEGIN -->
Implemented (uncommitted): src/vpk_zip.c + include/vpk_zip.h (zip reader), src/sshd_vpk.c (list/install), cmd_vpk row in sshd_builtins.c, SSHD_TITLEID compile definition from VITA_SERVER_TITLEID in CMakeLists.txt, AGENTS.md updated. Host test `python3 -I scripts/vpk_zip_test.py` passes (ASan/UBSan; stored, deflate, bad CRC, truncated, unsafe names, encrypted, bzip2, bad offset, sink abort). `task build` produces both VPKs with no warnings.

Design departure from the plan: commit moves each replaced file aside as <name>.vpkold and rolls every rename back on failure, rather than only deleting the stage dir, so the app is all-old or all-new even if a rename fails mid-commit. eboot.bin is committed first so a locked eboot fails with nothing to undo.

Not verified, needs a device: AC1, AC5, AC6, AC7, AC8. Open hardware questions: can this non-UNSAFE app rename/replace files under ux0:app/ROM000070, and can it replace its own running eboot.bin. If not, the commit root switches to ux0:patch/ROM000070 (AC8). Also unverified on device: sceIoRename onto a path in another directory on ux0, and the 0x3001 devctl free-space check on 'ux0:' (a failed devctl is treated as unknown and does not block).

Host test script renamed to scripts/test_vpk_zip.py (run: python3 -I scripts/test_vpk_zip.py); earlier note refers to it as vpk_zip_test.py.

Host test C file renamed to scripts/test_vpk_zip.c to match.

Hardware run 2026-11-16 (work network, port 22). Safe-mode build: ux0:app and ux0:patch invisible (ENOENT), df gives no device info. Switched the server SELF to UNSAFE (CMakeLists.txt); ux0:app/ROM000070 is then visible and df works. Side effect: the server now binds port 22 (SSHD_PORT_PRIMARY) instead of falling back to 2222; the user chose to keep 22. smoke.sh, CLAUDE.local.md and README still say 2222.

Findings: `which vpk`, `help vpk`, `vpk list` (title ID + files) work on device (AC2, AC9 hold). Fixed on device: %zu printed 'zu' in sh_printf; remove_tree treated a missing dir as an error because opendir answers ENOTDIR for a missing path here. After the fix the install stages and CRC-checks, then fails at the commit with 'eboot.bin: cannot move the installed file aside: Permission denied', rolls back, removes the stage dir, and leaves the app and ux0:data/vitasshd untouched (partial evidence for AC6 and AC5).

AC8 trigger: ux0:app/ROM000070 is not writable by the app even when UNSAFE. touch, cp into it and mv of its eboot.bin all answer Permission denied; this is the directory, not a lock on the running binary. mkdir /ux0/patch succeeds (removed again). Whether ux0:patch/ROM000070 can be created and whether the Vita overlays it at launch is untested. In-place install (AC1 as written) is not possible on this device without a different mechanism.

Unrelated bug seen: SFTP upload over an existing file did not truncate (scp of a 1359770-byte file over a 1359776-byte one left 1359776 bytes); rm first, then upload, worked. Not part of this task.

Patch overlay result 2026-11-16: `vpk install` wrote the full package to ux0:patch/ROM000070 (5 files, base ux0:app copy and ux0:data/vitasshd untouched), the user quit and relaunched the app, and sshd.log still logged the old build stamp (sftp_server.c built 17:46:30; the patch eboot carries 17:50:07, confirmed from the local ELF). The Vita did not launch from the dropped-in overlay, so AC8's fallback does not work as a plain directory copy; it probably needs the patch registered with the system (promoter), not just files in place. Removed /ux0/patch/ROM000070 from the device afterwards. Candidate route: unpack to the stage dir and hand it to scePromoterUtil (what VitaShell's installer does), which needs SceLibPromoter and the PROMOTER_UTIL module; the app is already UNSAFE. Not started, needs the user's decision.

The committed vpk install (stage, commit with .vpkold backups and rollback, patch root) is tested on device only as far as: stage + CRC, commit into a writable directory, patch dir creation, failure rollback against a read-only dir. It does not yet produce a new running build.

Promoter route research 2026-11-16 (read from VitaShell master package_installer.c, fetched with curl; Context7 /theofficialflow/vitashell only has build files, TinyFish fetch returned 401 until the user logged in): VitaShell's sequence is extract the VPK to ux0:data/pkg, write sce_sys/package/head.bin, load the PAF module (sceSysmoduleLoadModuleInternalWithArg, SCE_SYSMODULE_INTERNAL_PAF, argp {0x180000,-1,-1,1,-1,-1}), load SCE_SYSMODULE_INTERNAL_PROMOTER_UTIL, scePromoterUtilityInit, scePromoterUtilityPromotePkgWithRif(dir, 1), Exit, unload both. head.bin is a 1072-byte template (resources/head.bin) with the content ID written at 0x30 and three SHA-1-derived 16-byte MACs (fpkg_hmac) patched in at offsets read from the header. SDK has psp2/promoterutil.h and libScePromoterUtil_stub.a. BLOCKER for the decision: head.bin is VitaShell's GPLv3 resource file and this repo is Unlicense with a README statement that no third-party code is vendored; not copied. Awaiting the user's choice.

Promoter run 2026-11-16 18:08: on the device `vpk install` unpacked and CRC-checked 5 files into ux0:data/pkg, wrote head.bin, then the system installer returned 0x80101009; ux0:data/pkg was removed and the installed app (eboot.bin 1362445 bytes, 18:06) and ux0:data/vitasshd were unchanged. The failing call was not named; added a stage name to the message (load PAF module, load promoter module, promoter init, promote package) and rebuilt. Hypotheses to test, not conclusions: the promoter refuses to replace an app that is running, or head.bin/param.sfo is rejected.

Promoter run 2 (18:10) with stage names: the system installer fails at "promote package" with 0x80101009; PAF and promoter modules load and init fine. Cause found in my code: the package's param.sfo has CONTENT_ID present but empty (length 1), sfo_string returned success, so the EP9000-<TITLEID>_00-0000000000000000 fallback never ran and head.bin carried a blank content ID. VitaShell falls back on an empty string. Fixed in package_open; rebuilt. Unconfirmed that this was the cause of 0x80101009; the other hypothesis (the system refuses to replace a running app) is still open.

Promoter run 3 (18:18) after the CONTENT_ID fix: the installer now fails at "promote package" with 0x80101114 instead of 0x80101009, so the empty CONTENT_ID was one real defect and the installer got further. 0x80101114 is documented only by community reports, not by the SDK headers: VitaShell issue #213 (https://github.com/TheOfficialFloW/VitaShell/issues/213, VitaShell's own VPK installed from inside VitaShell gives 0x80101114; the workaround given is to install it from a different app, molecularShell) and a reddit thread titled 'vitashell error 0x80101114' saying a VitaShell VPK cannot be installed from VitaShell itself. Reading: the promoter refuses to replace the app that is currently running. Not tested directly from this app (that needs a package for a different title ID, which package_check refuses). If right, a running server cannot install itself through the promoter, and neither the file-copy, patch-overlay nor promoter route lets the running app update itself; an install would have to be done by a different running app. AC1, AC4, AC6, AC7 and AC8 need rewriting or the task needs a different shape; waiting on the user.

Running-app test 2026-11-16 18:2x (option 3): built a throwaway server variant (app ROM000070, package check hard-coded to ROM000071; not committed, CMakeLists.txt restored), plus a copy of the real VPK with param.sfo TITLE_ID patched to ROM000071 and the title text to VitaTest. Installed the variant by VitaShell, then `vpk install` of the test package from the running server: unpacked, handed to the promoter, returned success, and ROM000071 appeared in /ux0/app (eboot.bin and sce_sys present). So the pipeline (stage, head.bin with the CONTENT_ID fallback, PAF + promoter modules, PromotePkgWithRif) works from this app; the only difference from the failing runs is that the target app was not running. Conclusion: the promoter refuses to replace the running app (0x80101114), and a running server cannot install its own update. Side effect on the device: a VitaTest bubble (ROM000071) to be deleted from LiveArea by the user; my uploaded test VPKs were removed from /ux0/data. The normal build was rebuilt and uploaded as /ux0/data/ssh_server_vita.vpk (1360912 bytes). Side finding: the same mechanism lets the server install a different app's VPK, for example the client ROM000069, if the title check were widened.

CURRENT STATE as of 2026-11-16 (the user paused to investigate further; nothing committed, status left In Progress, ACs and plan above are unchanged and partly superseded by the notes in order below).

Code in the working tree (uncommitted): src/vpk_zip.c + include/vpk_zip.h (zip reader), src/vpk_head.c + include/vpk_head.h (head.bin builder), src/sshd_vpk.c (`vpk list` and `vpk install` via the promoter), cmd_vpk in src/sshd_builtins.c, sh_vpk_* declarations in include/sshd.h, CMakeLists.txt (UNSAFE SELF, SSHD_TITLEID define, generated build/vpk_head_template.c, ScePromoterUtil_stub, new sources), third_party/vitashell/{head.bin,LICENSE,README.md} (VitaShell GPL-3.0, vendored with the user's go-ahead), scripts/test_vpk_zip.{py,c} and scripts/test_vpk_head.{py,c}, README.md (provenance, licence, shell command list, vpk paragraph), AGENTS.md. Both host tests pass; `task build` is clean.

AC status by evidence: #2 and #9 verified on a device (`vpk list`, `help vpk`, `which vpk`). #3 partly: the title-ID refusal was exercised only indirectly (ROM000071 accepted by the throwaway variant, ROM000070 refused path not run); the eboot checks have not been run on a device. #5 holds so far: no run touched ux0:data/vitasshd except the earlier removed stage dir. #6 partly: stage-time and promoter failures left the installed app and ux0:data/pkg clean on the device; a client disconnect mid-unpack and a full card were not tried. #1, #4, #7, #8 are not met: the running server cannot install itself by any route tried (read-only ux0:app, ignored ux0:patch copy, promoter 0x80101114 on the running app).

What works on a device: the full stage + head.bin + promoter pipeline installs a package for a different, non-running title (ROM000071 test, confirmed in /ux0/app).

Open questions for the investigation: (1) is 0x80101114 really 'target app is running'; only community reports plus the ROM000071 control support it, no SDK or firmware documentation was found. (2) can the running app be made to not count as running, for example by installing with sync=0 and exiting, or by launching another app first; untested. (3) whether the server could instead update through a second app (client or a small helper) that promotes then relaunches the server; untested and larger than this task. (4) the narrower re-scope of installing the client (ROM000069) from the server needs only a wider title check; untested.

Device state to clean up: a VitaTest bubble (ROM000071) is installed and should be deleted from LiveArea; /ux0/data/ssh_server_vita.vpk holds the normal build (1360912 bytes) to reinstall through VitaShell, because the device was last known to be running the throwaway variant that accepts ROM000071. Work-network address and port: the work Vita (CLAUDE.local.md), port 22 (the UNSAFE build binds 22); smoke.sh, CLAUDE.local.md and README examples still say 2222.

Unrelated bugs seen, not part of this task: SFTP upload over an existing file does not truncate (rm first as a workaround); scp prints 'remote fsetstat: Operation unsupported' after every upload (documented in the README).

Bootstrap route research 2026-11-16 (upstream sources read with gh/curl; no device run yet). The running app CAN install its own update, but never by itself: the platform-sanctioned pattern is a second title that does the promote after the main app has exited.

Evidence, three shipping projects that do exactly this:
- VitaShell updater/main.c + network_update.c: a separate app (updater/, TITLE_ID VSUPDATER, its eboot.bin and param.sfo compiled into the main VPK as resources) is written to ux0:data/pkg, given head.bin and promoted — a different title, so promoting it from the running app is fine. Then sceAppMgrLaunchAppByUri(0xFFFFF, "psgm:play?titleid=<stub>") followed immediately by sceKernelExitProcess(0). The stub calls sceAppMgrDestroyOtherApp(), promotes ux0:data/pkg (the new VitaShell), then launches VITASHELL and exits.
- devnoname120/vhbb: same shape, UPDATER_TITLEID "VHBBUPDT1", built by a cmake create_updater() that embeds the stub's eboot and param into the main VPK; the main app is vita_create_self(... UNSAFE) like ours.
- Breezyslasher/Vita_plex and Vita_Suwayomi (2025), docs/in-app-updates.md, states it plainly: "A title can't promote over itself (ScePromoterUtility returns 0x80101114 in use)". That names the error from promoter run 3 and confirms the ROM000071 control. Their stub (src/updater_stub/main.cpp) reads fixed convention paths (ux0:data/<App>/update.vpk plus update_version.txt) "so no arguments need to cross the app boundary", and is deliberately minimal (headless vita2d, links only the install code). The exit must be immediate — sceKernelExitProcess(0), _Exit on PS4 — because an orderly quit that leaves threads alive still counts as in use. The technique is credited to AutoPlugin2.

All of it is in the public SDK: psp2/appmgr.h has sceAppMgrLaunchAppByUri, sceAppMgrDestroyAppByName, sceAppMgrDestroyOtherApp and sceAppMgrGetIdByName (a running check to use before promoting); libSceAppMgr_stub.a and libSceProcessmgr_stub.a ship in the SDK. Nothing here needs a plugin or a kernel exploit.

Ruled out: ux0:patch as a plain copy (already disproved on the device, and there is no SDK API to register a patch — grep of psp2 headers finds no install-patch call, and VitaShell's package_installer.c has no PATCH_NO path, only PromotePkgWithRif and PromoteImport). ux0:patch only works through a kernel plugin (rePatch/reLoaded, whose eboot patching was written for NoNpDrm games) which needs per-title tai config on the device: out of scope. ioPlus is not what its name suggests: ioPlus 0.1 hooks sceFiosKernelOverlayAddForProcessForDriver (0x17E65A1C) to turn "invalid:" overlays into identity mounts, so it is fio overlay elevation, not permission to write ux0:app.

Recommended shape: keep vpk list and vpk install for not-running targets as they are; add an updater stub (own add_executable, own vita_create_self and vita_create_vpk, own TITLE_ID and sce_sys dir, no custom icon so the 8-bit palette PNG trap cannot bite, linking only SceAppMgr, SceSysmodule, ScePromoterUtil and SceProcessmgr) packed into the server's VPK as a file the server reads from app0:. vpk install of the own title then means: stage and write head.bin in the server (code already written and host-tested), promote the stub, launch the stub by URI, flush the channel and queue exit-status and EOF, then sceKernelExitProcess(0). The stub promotes the staged directory and launches ROM000070 unconditionally, so a failed promote still leaves a bootable app (AC6, AC7). The staged path becomes a fixed convention baked into both binaries, and it should move off ux0:data/pkg (that is VitaShell's own staging dir) and out of ux0:data/vitasshd, where the host key and authorized_keys live (AC5). AC8, the patch overlay, should be dropped; AC1, AC4, AC6 and AC7 become testable with the stub.

Open for the user: whether vpk install accepts third-party titles. With the handoff in place the safe line is: our own titles always; another title only when sceAppMgrGetIdByName says it is not running; never destroy a third-party app over SSH.

Tree state correction: the vpk code is committed (6b6416f feat(sshd): add vpk list and install builtins), so the CURRENT STATE block above that says nothing is committed is out of date.

Resumed on devbox B from the home network; the user supplied the home Vita endpoint from CLAUDE.local.md. Initial SSH probe fails with 'No route to host' before authentication. Honcho MCP unavailable (HTTP 404 Session not found). No code edits. Proposed next implementation is the researched separate updater-title handoff, retaining own-title-only validation; awaiting explicit plan approval per workflow.

Updater implementation in working tree: bundled updater.vpk ROM000072 (avoids earlier test ROM000071), shared promoter loader, private ux0:data/vitasshd-update/{server,helper} stage, one-shot ready marker, result.txt, targeted shutdown of ROM000070 only, relaunch even on promote failure. SSH command drains status/EOF/close before URI launch and immediate process exit. Own-title-only package check retained. README explicitly does not promise atomic rollback or survival of unlisted app files under system promotion; obsolete AC4/AC8 remain unchecked pending reconciliation/hardware evidence. Tests: python3 -I scripts/test_vpk_zip.py, test_vpk_head.py, test_vpk_updater.py all pass; task build produces client/server/helper VPKs. Mini SDK initially lacked libvita2d, libpng, bzip2; installed these with vdpm (plus resolved dependencies), after setup built OpenSSL/libssh2/libssh. Client has existing unused-variable/function and strncpy warnings; new server/helper compile cleanly.

Networking: direct ssh from devbox B fails No route to host, nc succeeds; use -o 'ProxyCommand=nc -G 5 %h %p', BatchMode, ConnectTimeout=5, keepalive for device work. Host fingerprint verified against the user's MBP output. Old device exec calls emit output then remote close/Broken pipe exit 255; SFTP/scp works (known fsetstat unsupported warning). After user relaunched server, uploaded build/ssh_server_vita.vpk as /ux0/data/ssh_server_updater_bootstrap.vpk, confirmed via vpk list (ROM000070, 6 files incl updater.vpk). Running app says 'not this app' due prior temporary title-check variant. Asked user to install this bootstrap through VitaShell and relaunch before end-to-end self-update test. No claim of hardware handoff success yet.

HARDWARE SUCCESS on home network: the user installed /ux0/data/ssh_server_updater_bootstrap.vpk via VitaShell and relaunched. Built distinct update by touching src/sftp_server.c and task --force build, uploaded as /ux0/data/ssh_server_updater_test.vpk. Executed vpk install: staged 6 files, promoted updater ROM000072, SSH command returned cleanly with status 0, server exited. Initial reconnect during transition failed; subsequent reconnect succeeded and result.txt read 'promote package: 0x00000000'. SFTP downloaded app0:eboot.bin: SHA256 1b06d88364de5d64c10e2aad4723a1490b5a0d9d8e2ba6b4919859f235b0e1b6 matches update VPK and differs from bootstrap 3f99650d0daf57f59397178714fa19ef7b3e1a0bfd8f4528b9cfd859ffea9409. Latest sftp log stamp is Nov 16 2026 19:19:59 versus bootstrap 19:13:58. Authorized_keys and host public key checksums unchanged across handoff; public-key auth and existing host identity still work. AC1, AC2, AC5 and AC9 now checked; AC7 manual quit/relaunch persistence and remaining rejection/failure tests still to do. No private key contents retrieved.

Hardware rejection tests pass with generated small VPK fixtures: wrong TITLE_ID ROM000069, missing eboot.bin, 4-byte eboot.bin, and 4096-byte eboot.bin with bad magic. Each prints a clear refusal. Bad magic stages then aborts; no promoter call. Fixtures removed by SFTP. AC3 checked.

Follow-up preservation/repeat-install test currently BLOCKED: built /ux0/data/vpk_preserve_probe.vpk containing the known-good server VPK files plus harmless vpk_preserve_probe.txt. vpk install printed 'Unpacking 7 files (1419792 bytes)...', then devbox B SSH keepalive (5s, count 1) timed out, and new connections timed out during banner exchange. No 'Installing the updater' message was observed. Do not claim second promotion or AC4 passed. Asked the user what the device screen shows and to quit/relaunch if still on server. Need inspect stage/log after restart and distinguish slow staging/too-aggressive client keepalive from actual server stall. Known-good installed executable remains the successful first updater test unless this probe subsequently completed. Ready/result markers and staged tree should be inspected before retry.

User requested retry after stalled second install. Retry still fails during SSH banner exchange through nc proxy. ICMP succeeds (60.8 ms), TCP connect to the home Vita succeeds, but no SSH banner arrives. Direct ssh still gets local No route to host. No new install attempted; device application/listener remains unresponsive at SSH protocol level. Need confirmation of actual VitaSSHd process quit/relaunch or screen status, not merely network reachability.

Device reachable again after user's latest retry. Inspection found result.txt 'promote package: 0x00000000', app0:vpk_preserve_probe.txt present with expected content, and app0:eboot.bin still matches expected update hash. Thus the earlier apparently stalled probe install DID complete. Log: exec at 19:22:39; 'stopped reading for 30 s; dropping the session' at 19:28:10; server ready at 19:28:18 and again 19:29:46. Do not attribute this conclusively to keepalive: the exact stall cause remains unknown. Probe presence plus promote success establishes a second helper install/handoff succeeded despite client disconnect. Started reinstall of normal /ux0/data/ssh_server_updater_test.vpk (without probe) using ServerAliveInterval=0, ConnectTimeout=8 and nc proxy, background shell job sh_1e720ee510011zL5rQuWtI0UFb; await completion notification, do not poll. Next check marker preservation, app hash and credentials.

Normal repeat install completed cleanly (background SSH job exit 0, full staged/handoff output) with ServerAliveInterval=0. After automatic relaunch, result.txt again reports promote package: 0x00000000. Installed eboot hash matches expected update; authorized_keys and host public key hashes still unchanged. app0:vpk_preserve_probe.txt is now absent, though it was present after prior probe-package promotion. Conclusion: system promotion removes omitted app-directory files; AC4 as written is disproved, not satisfied. Must obtain user approval to revise AC4 to whole-package replacement and AC8 to updater handoff instead of the disproved overlay route. AC6's absolute guarantee for installer failures/full-card cases also exceeds app-controlled staging guarantees; do not mark checked. AC7 still needs explicit manual quit/relaunch evidence after known-good update (log shows restarts during probe episode, but no independent post-check hash following a confirmed manual restart). No code changes made in this retry segment.

The user approved revising obsolete criteria to documented system-promotion semantics: whole-package replacement may remove omitted files; separate updater rather than plain patch overlay; app controls staging failure cleanup, not system-installer rollback. Updated ACs accordingly. AC6 remains unchecked pending consolidated failure-path verification/review; AC7 remains pending a confirmed manual quit/relaunch followed by executable hash check. User's 'Yes' also follows request to quit/relaunch, but does not explicitly confirm restart occurred; verify rather than assume.

Final verification: the user explicitly confirmed 'Relaunched successfully'. Latest ready log at 19:34:30 is a distinct restart after repeat promotion at 19:31:11. Post-relaunch SFTP hash of app0:eboot.bin still matches expected update; authorized_keys and host public key hashes unchanged. Stage listing contains only result.txt, no ready/server/helper leftovers. AC7 satisfied. AC6 evidence: hardware invalid-magic staging abort clearly refused installation; later installed hash remains expected and staging cleaned; host stub tests cover targeted-shutdown failure, promotion failure and server-launch failure, with result logging and relaunch attempt after failure. This is not a real full-card/system-installer rollback test; those absolute guarantees were explicitly removed with user approval. Removed only this run's /ux0/data/vpk_preserve_probe.vpk; retained bootstrap and known-good update VPKs for recovery. All 3 host tests rerun and pass; git diff --check clean. Changes remain uncommitted.
<!-- SECTION:NOTES:END -->

## Final Summary

<!-- SECTION:FINAL_SUMMARY:BEGIN -->
Implemented server self-update via a bundled separate updater title (ROM000072), resolving the Vita promoter's refusal to overwrite a running title. Server validates/stages only ROM000070 packages, installs its trusted helper, closes SSH cleanly, launches the helper and exits. Helper promotes and relaunches the server, with a one-shot handoff marker and on-disk result. Shared promoter handling and CMake package dependencies keep the helper small and bundled automatically.

Validation: task build produces client/server/helper VPKs; python3 -I scripts/test_vpk_zip.py, test_vpk_head.py and test_vpk_updater.py pass (host sanitizer tests); git diff --check passes. On hardware: distinct updated executable verified by SHA256/build stamp; automatic relaunch and repeated self-installs succeed; wrong-title/missing/short/bad-magic executable fixtures refused; authorized_keys and host identity unchanged; separate manual quit/relaunch retains the updated executable. Probe test established that omitted app files are removed by system promotion, and criteria/docs were revised with the user's approval.

Limits: one-time VitaShell bootstrap for older builds; no app-managed rollback of system-installer failures, no real full-card/power-loss test, and result.txt records the last helper run rather than pre-handoff failures. One probe install suffered a client-output stall but eventually promoted successfully; repeat install without aggressive client keepalives completed cleanly. Ordinary exec-mode SSH exit-255/close behavior remains outside this task. README and AGENTS.md updated; test uploads cleaned except recovery VPKs. No commit made.
<!-- SECTION:FINAL_SUMMARY:END -->
