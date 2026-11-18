---
id: TASK-036
title: Convert remaining British English spellings in prose to US English
status: To Do
assignee: []
created_date: '2026-11-18T07:18:00Z'
labels:
  - docs
  - chore
dependencies: []
references:
  - AGENTS.md
  - README.md
  - docs/vcmd.md
  - scripts/test_vpk_zip.py
modified_files:
  - AGENTS.md
  - src/sshd.c
  - src/sshd_hostkey.c
priority: low
type: chore
ordinal: 68000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
The project's written convention is US English in prose. README.md and docs/ were converted in the pass that introduced the convention; this task covers what is left, repo-wide.

SCOPE: prose only. Headings, paragraphs, list items, table cells and code comments. NOT identifiers, string literals, code fences, command examples, URLs or file names. Renaming something like normalise_stat() is a code change with the repo's build check, not a spelling fix, and is out of scope here.

METHOD: match whole words against a British->American word list and read each hit before changing it. NEVER blind-replace ou with o or ise with ize: should, would, could, ground, group, account, through, country and route are correct as written, and advise, supervise, exercise, comprise, advertise, promise and surprise are -ise in US English too. A word US English also accepts is not a British spelling to fix (dialogue is the US form; catalogue and catalog are both fine, prefer catalog).

WHERE THE HITS ARE (counts from a whole-word scan of tracked files, excluding third_party/ and .task/; re-measure before editing):
- AGENTS.md: honoured, Normalising, desynchronises.
- backlog task notes: the bulk of the remainder (behaviour, plus judgement, recognised, rumour, sanitised). One is a task title, so it shows in backlog task list and needs the title edited, not just the body.
- src/ comments: initialises, cancelled, authorising, enrolment, labelled and similar in comments only.
- .agents/skills/vita-port/** and .agents/skills/clang/**: the vendored skills collection, so fixes there have to go upstream too, or they are lost on the next sync.

EXPLICITLY EXCLUDED, do not "tidy" these:
- The login passphrase wordlist: grey and labour are live words in scripts/wordlist-pool.txt and the 1024-entry table in src/sshd_hostkey.c. Changing them changes passphrases the server issues. If they are ever converted it is its own commit, via scripts/gen_wordlist.py --emit, and scripts/gen_wordlist.py --check must pass.
- Quoted program output and server strings, e.g. the file(1) text 8-bit/color RGB, and Operation unsupported.
- Third-party and vendored text: third_party/vitashell/head.bin and its GPL-3.0 license text, .task/portlibs/**, and any upstream tarball source. Also the word Unlicense and LICENSE.md, which are names.
- Identifiers, and already-American words the scan may look suspicious (colormap, color type 3).
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 A whole-word British->American scan over tracked files (excluding third_party/ and .task/) reports zero hits in prose, with the recorded exclusions listed in the task notes
- [ ] #2 No identifier, string literal, code fence, command example, URL or file name was changed
- [ ] #3 The login passphrase wordlist in scripts/wordlist-pool.txt and src/sshd_hostkey.c is unchanged
- [ ] #4 markdownlint -f -c .markdownlint.jsonc reports no new errors against HEAD for every touched markdown file
- [ ] #5 Any comment-only edits under src/ are followed by task build producing both build/ssh_client_vita.vpk and build/ssh_server_vita.vpk
- [ ] #6 Every host check run for this task runs under timeout and exit 124 counts as failure, not a pass
<!-- AC:END -->
