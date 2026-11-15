---
id: TASK-004
title: Rework the README for the project's actual scope
status: To Do
assignee: []
created_date: '2026-11-15T22:57:00Z'
updated_date: '2026-11-15T23:49:49Z'
labels: []
dependencies:
  - TASK-001.01
  - TASK-002.03
documentation:
  - README.md
  - AGENTS.md
  - CMakeLists.txt
modified_files:
  - README.md
type: docs
ordinal: 11000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
The README still describes the upstream project: the feature and limitation lists are client-only, and there is no server section telling anyone how to connect to a Vita from a devbox. Someone arriving at this repo cannot tell from the prose that the interesting half exists. This repo is standalone, not a fork, so the README should say what it is and keep the upstream credit where it belongs — a provenance note, not a title.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 the README states what this repo is now (SSH client and SSH server apps for the Vita) and credits the upstream fork in a provenance note rather than as the title
- [ ] #2 each app is described separately with its own titleid and VPK name
- [ ] #3 the server section tells a user how to connect: what to install, where the password or key comes from, and one command line they can paste
- [ ] #4 the feature and limitation lists match what the code does today, removing claims that are no longer true
- [ ] #5 the prerequisites and build sections match the current task targets, including all portlibs the setup step builds
- [ ] #6 no section claims functionality the server does not yet have; unfinished parts are labelled as such
<!-- AC:END -->

## Status Updates
<!-- STATUS:BEGIN -->
### Update: [2026-11-15 23:49:49 UTC]
- Retitled from "this fork's actual scope": the repo is no longer a fork, and a worker
  following the old title would write "this fork" prose straight into the README. The
  description's matching claim is corrected too.
- Do not redo the parts already handled while the repo was de-forked: the H1 is now
  "VitaSSH – SSH client and server for PS Vita", a provenance note under it credits
  rompelhd's VitaSSH and TheOfficialFloW's VitaShell, and the Generates section names both
  apps with their VPK names and titleids. That leaves #1 partly met (the opening paragraph
  still describes only the client) and #2 partly met (titleids appear, but there is no
  per-app description), which is why neither is checked.
- What is genuinely left: the client-only feature/limitation lists (#4), the how-to-connect
  section (#3), prerequisites/build accuracy (#5), and labelling unfinished server behaviour
  (#6). #3 and #6 depend on TASK-002.03, so they cannot be settled before SFTP is verified.
<!-- STATUS:END -->
