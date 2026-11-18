---
id: TASK-024
title: 'Add a reboot builtin, with shutdown -r now as a Linux spelling'
status: To Do
assignee: []
created_date: '2026-11-17 05:14'
updated_date: '2026-11-18T07:47:00Z'
labels:
  - shell
  - builtin
  - power
  - safety
dependencies: []
references:
  - src/sshd.c
  - src/sshd_session.c
  - include/vpk_update.h
  - docs/vcmd.md
  - 'https://github.com/devnoname120/vitacompanion'
documentation:
  - AGENTS.md
  - docs/vcmd.md
modified_files:
  - src/sshd_builtins.c
  - src/sshd.c
  - README.md
  - docs/vcmd.md
priority: medium
ordinal: 55000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
Add 'reboot' to the server shell, with 'shutdown -r now' as a Linux-shaped spelling in front of it. This is the verb that turns "ssh in and fix it" into "ssh in and recover a wedged device" -- the case where the network stack or a plugin has hung and only a reset clears it. vitacompanion has it and it is the most-used command there.

Nothing about the API is hard. The whole task is ordering a reset against channel teardown while forbidden from sleeping.

API. scePowerRequestColdReset() is in USER psp2/power.h (line 160) and ScePower_stub is already linked (CMakeLists.txt:217). Add no libraries. Note for whoever wastes time on this: the public stub exports no shutdown or power-off -- the only scePowerRequest* calls are ColdReset, Standby, Suspend, DisplayOff, DisplayOn. So reboot is the only reset that exists; do not go looking for scePowerRequestShutdown, and do not substitute scePowerRequestSuspend, which suspends rather than resets and will not clear a wedged network stack. Whether ColdReset is permitted from an unsigned app is UNVERIFIED -- if it returns an error code, report the code and stop; do not fake success.

NO PROMPT, AND NONE IS POSSIBLE. Do not implement an interactive "are you sure?" and do not try to. A builtin's stdin is a bounded buffer loaded in full before the command runs (`sh_stdin_t`, from `cmd < FILE`, TASK-015.01), and a read that finds nothing returns end-of-input at once, so no builtin ever waits for a keystroke. The line editor consumes channel bytes as commands, and mid-command reads from the channel do not exist. That is a rule of the design, not a gap to fill: waiting on a human keystroke is an unbounded wait inside the server's single poll loop, the exact thing vcmd's five-second deadline exists to avoid. A future interactive stdin would need its own deadline design before a prompt became possible. Both verbs are non-interactive end to end, exactly like 'sudo reboot': 'ssh host "reboot now"' over a socket with no tty must work and exit 0.

SYNTAX. Two verbs, one engine. 'reboot' is this shell's own verb; 'shutdown' is a compatibility front-end over the same code, the same shape as TASK-017.03's 'wget as a frontend over the curl engine'. Give 'shutdown' its own row in builtins[] (alphabetically between 'rmdir' and 'stat') so it shows in 'help' and 'which shutdown' reports shell builtin -- a hidden alias is a rumor, not a command.

  reboot now         reset as soon as the sessions have flushed -- seconds, as immediate as sudo reboot
  reboot +90         reset ninety seconds out
  reboot cancel      abort a pending scheduled reset
  shutdown -r now    identical to 'reboot now'
  shutdown -r +90    identical to 'reboot +90'
  shutdown -c        identical to 'reboot cancel'

Parsing rules, and why they are strict:
  - '-r' is the only action 'shutdown' implements. Anything meaning stop-instead-of-reboot -- '-h', '-P', '-H', '-p', 'halt', 'poweroff' -- is refused with the reason above, and must never reboot.
  - A time argument is required by both verbs, which matches 'shutdown -r' with no time failing on Linux.
  - Unknown or unimplemented flags are refused BY NAME, not ignored. Accepting 'shutdown -r now --force' and quietly dropping --force is the same lie as '-h' rebooting. Where a flag has an honest mapping, map it: '-t N' is what '+N' already is, and '-k' (warn-only) is what scheduling already does -- so neither needs separate machinery.
  - DELIBERATE DIVERGENCE FROM LINUX, keep it and say so: bare 'reboot' does not reset, it prints a usage error. On Linux 'reboot' alone reboots because sudo/root is the gate; there is no sudo in front of this shell -- the ssh login IS the authentication -- so the command line is the only friction between a mistyped line and a device reset. Put that in the usage line and the README, so a Linux user is not quietly surprised the first time.

The broadcast Linux's shutdown sends to logged-in users is the warn-all-sessions requirement below; that is why '-k' needs no extra code.

This also converges with the implementation instead of fighting it: the reset is already deferred behind a deadline the poll loop watches (below), so a delay costs one number rather than new machinery -- and it buys the operation people actually want, "reboot in a minute" so a transfer can finish. When scheduling, report how many sessions are connected, because all of them go down with it.
Rejected for the record: a required literal 'confirm' word. It reads as a dialogue this shell cannot have, and as a safety device it is weaker than it looks -- shell history replays the whole line, argument included, so it never stopped a replay, only a mistyped verb. The guards below are what actually catch a mistake, because they look for evidence of a reason to hesitate instead of for a word to be typed.

THE DESIGN PROBLEM, solve this first. Do not call scePowerRequestColdReset() inline from the builtin. The client has to receive the message plus exit-status 0, EOF and a channel close before the SoC resets, and a builtin may not sleep to buy that time -- AGENTS.md is explicit that a running command never blocks, and it also warns that tearing the transport down early makes the client's ssh exit 255, so a script reads a successful reboot as a failure. The working shape is: the builtin records intent (a process-global volatile flag plus a deadline), the command returns, and the server loop issues the reset once the pending sessions have finished flushing or the deadline arrives. Reuse the pacing that already exists for exec output (chan_write_raw(), wait_for_room(), sh_session_pump()) rather than inventing a flush, and follow the exit-status-then-close sequence already at src/sshd_session.c:1475-1502.
  TRAP: 'quit_requested' (src/sshd.c:90) looks like exactly the flag to copy -- it is the server loop's condition at :543 -- but it is never assigned anywhere in the tree. It is vestigial. The real exit path is 'if (start_pressed()) { sh_console_log("START held: stopping"); break; }' at src/sshd.c:583-586. Build a real flag; do not rely on that one, and treat noticing it as a separate cleanup, not part of this task.

Guard every path that can lose something, each with its own message naming what it found. The guards do the safety work the rejected magic word was pretending to do, because a staged update or an in-flight transfer is evidence, and a typed word is not.
  - A staged update. If VPK_UPDATE_READY exists, or anything lives under VPK_UPDATE_ROOT (include/vpk_update.h is the path contract), refuse: resetting between stage and promote leaves a stage and a one-shot ready marker in a state that contract does not describe.
  - Low battery. scePowerIsLowBattery() is already used at src/sshd.c:599. A device that dies mid-boot is the one failure you cannot recover remotely, so refuse on low battery and say why. A documented override is acceptable; silence is not.
  - An in-flight SFTP transfer. Investigate whether the server can cheaply tell (sftp sessions are tracked via sh_sftp_pump()/sh_session_pump()). If it can, warn or refuse; if it cannot, record that in docs/vcmd.md and do not pretend the case is handled -- an interrupted write is exactly how silent corruption like TASK-022 appears.
  Refusals apply to 'now'. For '+90' they are warnings instead, because the delay may be exactly what resolves them.

Log the intent BEFORE resetting: sh_console_log() with the requesting peer and the mode as typed ('shutdown -r +90' and 'reboot +90' are different lines to a reader afterwards), so that if the console does not come back there is a line in sshd.log saying what was asked and by whom. After a reboot that fails to boot, that line is the only evidence anyone will have.

Warn everyone, not just the asker. All connected sessions get a line saying a reset is coming and roughly how long they have -- at schedule time for '+N', and again shortly before the reset. One reset for N sessions.

The message must say the device will not come back on its own. This server is an app, so nothing relaunches it after a reboot -- someone has to press the icon. Say that in the command's own output and in README, because the alternative is a user staring at a dark livearea concluding they bricked a device they only asked to reboot.

Register both rows in builtins[] in alphabetical position: 'reboot' between 'pwd' and 'rm' (after 'quit' if TASK-023.04 landed first), 'shutdown' between 'rmdir' and 'stat'.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 'reboot now' resets the console with no prompt and no tty, driven as a single scripted 'ssh host "reboot now"', and the client exits 0 -- verified on a device.
- [ ] #2 Bare 'reboot' neither prompts nor resets: it prints a usage line saying what would happen and when, and states that it deliberately does not act with no argument.
- [ ] #3 'reboot +90' resets about ninety seconds later and 'reboot cancel' aborts it before then, verified on a device.
- [ ] #4 The client receives the full message plus exit-status 0 and a clean channel close -- ssh exits 0, not 255 -- verified against a scripted invocation.
- [ ] #5 Every other connected session prints the warning before the reset, and scheduling reports how many sessions are connected; verified with two simultaneous sessions.
- [ ] #6 The intent is in sshd.log with the requesting peer and the spelling as typed, before the reset happens, verified by reading the log after the device is back.
- [ ] #7 A staged update under VPK_UPDATE_ROOT blocks 'reboot now' with a message naming what it found, verified by staging one and not promoting it; for '+90' the same condition warns rather than refuses.
- [ ] #8 Low battery is refused with the reason, and any override is documented rather than silent.
- [ ] #9 The interrupt behavior of an in-flight SFTP transfer is investigated and the finding is recorded in docs/vcmd.md either way.
- [ ] #10 'shutdown -r now' behaves identically to 'reboot now', verified on a device; it has its own builtins[] row, appears in 'help', and 'which shutdown' reports shell builtin.
- [ ] #11 'shutdown -h now' refuses with the no-power-off reason and does NOT reboot, verified on a device, as do '-P', '-p', 'halt' and 'poweroff'.
- [ ] #12 'shutdown -c' cancels a scheduled reset, verified on a device.
- [ ] #13 An unimplemented flag is refused by name rather than silently ignored, verified with at least one example such as '--force'.
- [ ] #14 The command's own output and README state that nothing relaunches the server after the reset and the app must be started by hand.
- [ ] #15 help lists both verbs with their usage, builtins[] stays alphabetically sorted, and task build is clean.
<!-- AC:END -->
