---
id: TASK-009
title: Give the client's tile an SVG source
status: To Do
assignee: []
created_date: '2026-11-16T00:00:00Z'
updated_date: '2026-11-17 03:46'
labels: []
dependencies: []
documentation:
  - assets/sshd-icon0.svg
  - assets/sshd-bg0.svg
  - sce_sys/icon0.png
  - sce_sys/livearea/contents/bg0.png
  - AGENTS.md
ordinal: 16000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
The server's tile and livearea background are rendered from assets/sshd-icon0.svg and assets/sshd-bg0.svg, and each header names the rsvg-convert and ImageMagick commands that produce the committed PNG. The client's sce_sys/icon0.png and sce_sys/livearea/contents/bg0.png arrived as PNGs with no source in the repo, so they cannot be regenerated or restyled, and both AGENTS.md and the README say the tiles are rendered from SVGs in assets/ — true for one app and not the other. Owning the art means having the file it comes from.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 assets/ holds SVG sources for the client's icon0 and livearea background, in the same convention as the sshd ones
- [ ] #2 each source names its render command in a header comment, and running it produces the committed PNG (or replaces it where the SVG is the corrected original)
- [ ] #3 the client and server tiles still read as one family when placed side by side
- [ ] #4 the tile claim in README.md and AGENTS.md is true for both apps
- [ ] #5 task build produces both VPKs
<!-- AC:END -->

## Implementation Notes

<!-- SECTION:NOTES:BEGIN -->
2026-11-16: Archived at the user's direction. The client is not being developed further; the sshd server and its shell are the project's primary focus. What stays true and untracked: the client's sce_sys/icon0.png and sce_sys/livearea/contents/bg0.png are committed PNGs with no SVG source in the repo, so they cannot be regenerated or restyled, and the claim in README.md and AGENTS.md that tiles are rendered from SVGs in assets/ holds only for the sshd app. The server's assets/sshd-icon0.svg and assets/sshd-bg0.svg are unaffected.
<!-- SECTION:NOTES:END -->
