---
id: TASK-023
title: 'Give the shell native system-control verbs: launch, quit, screen, nosleep'
status: To Do
assignee: []
created_date: '2026-11-17 05:09'
updated_date: '2026-11-17 05:14'
labels:
  - shell
  - builtin
  - power
  - appmgr
dependencies: []
references:
  - docs/vcmd.md
  - 'https://github.com/devnoname120/vitacompanion'
documentation:
  - AGENTS.md
  - docs/vcmd.md
modified_files:
  - src/sshd_builtins.c
  - README.md
priority: medium
ordinal: 50000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
Goal: four new server builtins -- launch, quit, screen, nosleep -- so the device can be driven from an ssh session without installing vitacompanion. Each is its own subtask; each is independently buildable and reviewable.

WHY THESE FOUR AND NOT MORE. vitacompanion exposes eleven verbs. Four of them are genuinely available to this app because the Vita APIs they need are user-mode and this server already links the stubs (SceAppMgr_stub at CMakeLists.txt:214, ScePower_stub at :217, SceCtrl_stub at :207 -- no new link lines are needed for any subtask). They are: launch, quit, screen, nosleep.

press and release are NOT possible and must not be attempted. Input injection is ksceCtrlSetButtonEmulation() / ksceCtrlSetAnalogEmulation() / ksceCtrlRegisterVirtualControllerDriver(), which exist only in psp2kern/ctrl.h (lines 163, 185, 201); user psp2/ctrl.h has nothing comparable. vitacompanion ships a .skprx for exactly this and is loaded by taiHEN into every app under *main. This server is an app (vita_create_self(... UNSAFE), CMakeLists.txt:224), so when the app you would drive becomes foreground this server is suspended and destroyed -- there is no session left to type with. Do not reach for sceCtrlSetRapidFire as a workaround: it is a repeating rule, buttons only, and applies while this app owns input.

promote is already superseded by the existing 'vpk install'. version is TASK-021's job. help exists. wait only exists to chain a scripted protocol.

RELATED, not a subtask: reboot is TASK-024. It is filed beside this parent rather than as a fifth subtask because its difficulty is not the API call -- scePowerRequestColdReset() is one user-mode call -- but deferring the reset until every session has flushed, which is a different problem from the four verbs here.

READ FIRST. docs/vcmd.md, section "Where the plugin boundary is, and what cannot be built in" -- that is the authority for what is possible and why; if you find something there to be wrong, correct it in the same change. AGENTS.md, "Adding a builtin command", for the house rules: run inside the single poll loop, never block, never chdir(), never printf, output through sh_printf()/sh_eprintf() in the form '<name>: <arg>: <strerror(errno)>', add a row to builtins[] in alphabetical order (help prints the table in order), then 'task build'.

NAMING, already decided: plain verbs in builtins[], not subcommands of vcmd. vcmd is a transport (HOST PORT [LINE]); letting 'vcmd press cross' sometimes mean "send these bytes to :1338" and sometimes "act locally" makes a silent no-op indistinguishable from success. If a subcommand shape is ever wanted, the in-tree model is 'vpk list FILE | vpk install FILE' -- a builtin owning a noun.

SHARED ACCEPTANCE for every subtask: 'task build' is clean; the new row sits in the right alphabetical position in builtins[]; 'help' lists the verb and 'help <verb>' prints its usage; 'which <verb>' reports shell builtin; every error message names the command and the offending argument; and behaviour is verified on a device, since compile-and-link proves nothing about these APIs.

SUBTASKS. The numbers came out of ordinal order when they were filed, so work from the titles and not from the numbering.

  TASK-023.01  nosleep on | off | status      (low)
  TASK-023.02  launch <TITLEID>               (medium)
  TASK-023.03  screen off | on | status       (medium)
  TASK-023.04  quit <TITLEID> | all           (medium)

Suggested order of work: launch, then quit (it wants a title id already proven to launch), then screen, then nosleep. Each is still deliverable on its own.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 The four subtasks are implemented, each building cleanly and each verified on a device.
- [ ] #2 press and release are not implemented, and docs/vcmd.md still explains why.
- [ ] #3 builtins[] stays alphabetically sorted and help output still reads in order.
- [ ] #4 No subtask adds a link library to the server target; if one seems necessary, that is a finding to raise, not a fix to make.
- [ ] #5 docs/vcmd.md's feasibility table is updated to say which of these verbs now exist.
<!-- AC:END -->
