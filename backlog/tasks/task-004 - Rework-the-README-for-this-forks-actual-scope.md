---
id: TASK-004
title: Rework the README for this fork's actual scope
status: To Do
assignee: []
created_date: '2026-11-15T22:57:00Z'
updated_date: '2026-11-15T22:57:00Z'
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
The README still describes the upstream project: the title credits it to the original author, the feature and limitation lists are client-only, and there is no mention of the server app, its titleid, or how to connect to a Vita from a devbox. Someone arriving at this repo cannot tell that the interesting half exists, and the provenance line reads as though upstream is still the reference. The repo's position is that this fork is canonical, so the README should say what this repo is and keep the upstream credit where it belongs — a provenance note, not a title.
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
