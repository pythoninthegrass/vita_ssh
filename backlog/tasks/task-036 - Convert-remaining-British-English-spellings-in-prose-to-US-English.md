---
id: TASK-036
title: Convert remaining British English spellings in prose to US English
status: Done
assignee:
  - '@claude'
created_date: '2026-11-18T07:18:00Z'
updated_date: '2026-11-18T07:49:00Z'
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
- [x] #1 A whole-word British->American scan over tracked files (excluding third_party/ and .task/) reports zero hits in prose, with the recorded exclusions listed in the task notes
- [x] #2 No identifier, string literal, code fence, command example, URL or file name was changed
- [x] #3 The login passphrase wordlist in scripts/wordlist-pool.txt and src/sshd_hostkey.c is unchanged
- [x] #4 markdownlint -f -c .markdownlint.jsonc reports no new errors against HEAD for every touched markdown file
- [x] #5 Any comment-only edits under src/ are followed by task build producing both build/ssh_client_vita.vpk and build/ssh_server_vita.vpk
- [x] #6 Every host check run for this task runs under timeout and exit 124 counts as failure, not a pass
<!-- AC:END -->

## Implementation Plan

<!-- SECTION:PLAN:BEGIN -->
## Where this runs

Worktree branch `task-036-us-spelling` off 6618e4d (`~/.local/share/opencode/worktree/5987c8/task-036-us-spelling`), so main's uncommitted alias work is untouched. The Backlog MCP follows the session directory, so task-record edits land in this worktree's `backlog/` too.

## Baseline scan

Whole-word scan over `git ls-files` (excluding `third_party/` and `.task/`) with an explicit British->American list, no blind `ou`->`o` or `ise`->`ize`: **247 hits in 72 files** at 6618e4d. `docs/` is already clean, so the earlier pass covered it.

## Classification, decided per hit by reading it

In scope: markdown prose (headings, paragraphs, list items, table cells), C/Python/shell comments, Python docstrings (documentation, not a value any code matches).

Out of scope, recorded as exclusions rather than silently skipped: identifiers; string literals and data a program or user matches on (shell test labels, the JSON values in `.agents/skills/vita-port/evals/evals.json`); fenced code and command examples; URLs and file names; the login passphrase wordlist (`grey`, `labour` in `scripts/wordlist-pool.txt` and the table in `src/sshd_hostkey.c`); quoted program output and server strings; third-party and vendored text; the names `Unlicense` and `LICENSE.md`.

## Steps

1. `AGENTS.md` (9) and `README.md` (12). README's `## Licence` heading and its `[Licence](#licence)` link are one rename: the heading becomes `## License` and its own fragment follows it. That is our own anchor, not an external URL.
2. Comments only under `src/` and `include/`: `sshd.c` (2), `sshd_session.c` (2), `sshd_builtins.c` (2), `sshd_hostkey.c` (5 comments; the 2 wordlist entries stay), `main.c` (1), `sh_textops.c` (2), `include/sftp_server.h` (1), `include/sh_grep.h` (1), `scripts/test_sh_headtail.c` (1). No string literals, no code.
3. `scripts/`: `convert_png.py` (4), `sftp_probe.py` (1), `smoke.sh` (2 comments; the `check` label string stays). `throughput.sh` is 0: all four hits are the `summarise()` function name.
4. `taskfiles/setup.yml` (1 comment) and `libs/libssh/vita.patch` (1 comment in a `+` line); verify the patch still applies against the extracted libssh source.
5. `assets/sshd-bg0.svg`, `assets/sshd-icon0.svg` (3, comments only, so no re-render).
6. Backlog task records through `task_edit` only, never by editing the markdown: ~20 active files. TASK-019.02's title carries `behaviour`, so its title is edited, which renames its file.
7. This task's own description quotes the British words it is about. Those are mentions of the words, not prose to convert, so they stay and are recorded as an exclusion.
8. Re-scan; expect zero hits outside the recorded exclusions.

## Checks, each under `timeout` with exit 124 treated as failure

- `markdownlint -f -c .markdownlint.jsonc` on every touched markdown file, compared against HEAD for new errors only.
- `task build` producing `build/ssh_client_vita.vpk` and `build/ssh_server_vita.vpk`. The baseline at 6618e4d already passed in this worktree before any edit, so a failure after editing is mine.
- `python3 -I scripts/gen_wordlist.py --check`, plus `git diff` proving the wordlist pool and the hostkey table are untouched.
- `patch --dry-run` for `libs/libssh/vita.patch`.
- The host test scripts for anything touched that has one.

## Risks

- main's other agent is editing `AGENTS.md`, `README.md`, `scripts/smoke.sh`, `src/sshd_builtins.c`, `src/sshd_session.c` and tasks 025/030/035 right now, partly with these same spelling fixes. Identical text merges cleanly; differing text on one line is a trivial conflict. Re-scan against main once their work lands.
- `.agents/skills/**` is pinned by `skills-lock.json` `computedHash`, so a local edit there drifts from the lock and the next skills sync reverts it.

## Decisions taken (2026-11-18, with the maintainer)

- `.agents/skills/**` (15 files, ~80 hits) is left alone and recorded as a vendored-text exclusion. It is hash-pinned in `skills-lock.json`, so an in-repo fix would drift from the lock and revert on the next skills sync.

- Backlog scope is the active tasks in `backlog/tasks/` only. `backlog/completed/` (1 file, 4 hits) and `backlog/archive/` (4 files, 5 hits) are left as written and recorded as exclusions: they are frozen records of finished and canceled work, and editing the archived ones would mean restoring them, which bumps a date meant to be settled.
<!-- SECTION:PLAN:END -->

## Implementation Notes

<!-- SECTION:NOTES:BEGIN -->
## Recorded exclusions (the hits a whole-word scan still reports, by file)

Final scan over tracked files (excluding `third_party/` and `.task/`): **125 hits in 24 files**, every one of them listed here. Zero remain in prose the repo owns.

- `.agents/skills/**` — 86 hits in 15 files (`clang/`, `vita-port/`). Vendored skills collection, hash-pinned by `computedHash` in `skills-lock.json`, so an in-repo fix drifts from the lock and reverts on the next skills sync. Fix upstream or not at all. `evals/evals.json` in there is also data, not prose.
- `backlog/completed/task-021 …` — 4 hits. Frozen record of finished work; scope is `backlog/tasks/`.
- `backlog/archive/tasks/` — 5 hits across `task-005.02`, `task-006`, `task-007`, `task-008`. Frozen records; editing an archived task means unarchiving it, which bumps a `completed_date` meant to be settled.
- This task's own record — 22 hits. It quotes the words it is about (`behaviour`, `normalise_stat()`, `honour`, `licence`, `analyse`, `catalogue`…). Mentions, not prose to convert.
- `src/sshd_hostkey.c` — 2 hits: the passphrase words `"grey"` and `"labour"` in the 1024-entry table, plus their source lines in `scripts/wordlist-pool.txt` (unchanged). Converting them changes the passphrases the server issues.
- `scripts/smoke.sh` — 2 hits, both string literals: the `check`/`skip` labels the harness prints (`honours`, `behaviour`). Not prose.
- `scripts/throughput.sh` — 4 hits, all the `summarise()` identifier and its call sites. Renaming it is a code change with the repo's build check, out of scope.

Also untouched, from the description's exclusion list: quoted program output and server strings, `third_party/vitashell/head.bin` and its GPL-3.0 text, the names `Unlicense` and `LICENSE.md`, and already-American forms the scan flags as suspicious (`colormap`, `color type 3`).

## Verification run (2026-11-18, every command under `timeout`, exit 124 treated as failure)

- Rebased onto `main` at `ceb4134` after TASK-018 closed there; the other agent's `eda39f5` had already fixed most of the code and doc hits, so the remaining set was mostly `backlog/tasks/`.
- `task build` produced `build/ssh_client_vita.vpk` (1102193 B), `build/ssh_server_vita.vpk` (1427452 B) and `build/ssh_updater_vita.vpk`. Baseline before any edit, and again after.
- `arm-vita-eabi-size build/ssh_server_vita`: text 2301384 — unchanged from the baseline build, as expected for comment-only edits under `src/`.
- Host checks, all pass: `test_vpk_zip`, `test_vpk_head`, `test_vpk_updater`, `test_sh_grep`, `test_sh_alias`, `test_sh_chain`, `test_sh_headtail`, `test_sh_textops`, and `gen_wordlist.py --check`.
- `git diff main -- src include scripts/test_sh_headtail.c` shows no changed line that is not a comment. `git diff main -- scripts/wordlist-pool.txt` is empty.
- `libs/libssh/vita.patch` `Honor` line verified by extracting the pinned `libssh-0.12.2` tarball and applying the patch with `patch -p1`.
- markdownlint compared against `main` over `AGENTS.md`, `README.md`, `docs/` and `backlog/tasks/`: 88 issues before, 88 after, byte-identical lists, so no new errors. All 88 are pre-existing (38 in `README.md`, 18/16/10 in `docs/throughput.md`/`turbo-wifi.md`/`turbo-storage.md`, 5 in `AGENTS.md`, 1 in `docs/vcmd.md`).

## Two things worth knowing

- `markdownlint -f` is `--fix`, not "force". The first check run used the flag as written in the global convention and it rewrote 30 markdown files in place (it strips the space before an inline code span, so `no \`chdir()\`` became `no\`chdir()\``). Everything was reverted from git and the check re-run without `-f`. AC #4 as written cannot be run literally; the check is `markdownlint -c .markdownlint.jsonc <files>`.
- The Backlog CLI's acceptance-criteria rewrite re-serializes a task file and moves sections (a `DOD` block landed after `NOTES`), and `--check-ac` does not survive a replacement in one invocation. So AC lines, `STATUS` blocks and hand-written sections were edited by exact-line whole-word replacement instead, and only `DESCRIPTION`/`PLAN`/`NOTES`/`FINAL_SUMMARY`/titles went through the CLI. Verified afterwards by comparing every changed file against `HEAD` as a multiset of lines with the recorded replacements applied: 0 unexpected differences, and AC checkbox states unchanged.
<!-- SECTION:NOTES:END -->

## Final Summary

<!-- SECTION:FINAL_SUMMARY:BEGIN -->
Converted the remaining British English spellings in prose to US English across the repo, on the `task-036-us-spelling` worktree branch, in two commits on top of `main` at `ceb4134`:

- `1ea48e3 style: use US English spelling in prose` — 16 files: `AGENTS.md`, `README.md` (including the `## Licence` heading and the link fragment that points at it), comments in `src/` and `include/`, docstrings and comments in `scripts/`, a comment in `taskfiles/setup.yml`, the `Honor` line in `libs/libssh/vita.patch` (verified to still apply against the pinned libssh tarball), and the comments in the two server SVGs.
- `cef98df docs(backlog): use US English spellings in active task prose` — 30 files under `backlog/tasks/`, descriptions, plans, notes, acceptance criteria, status updates and final summaries. Two titles changed, which renamed their files: task-002.02 (`cannot honour` → `cannot honor`) and task-019.02 (`real device behaviour` → `real device behavior`).

The final whole-word scan reports 125 hits in 24 files, every one an exclusion recorded in the notes with its reason: the vendored `.agents/skills/**` tree (86, hash-pinned), the frozen `backlog/completed` and `backlog/archive` records (9), this task's own record quoting the words it is about (22), the login passphrase wordlist (2 in `src/sshd_hostkey.c` plus its pool file), the two `scripts/smoke.sh` label strings, and the `summarise()` identifier in `scripts/throughput.sh` (4). Nothing the repo owns has a British spelling left in prose.

Verification: `task build` produces all three packages, before and after; every host check passes under `timeout`; `gen_wordlist.py --check` passes and the wordlist files are untouched; `arm-vita-eabi-size` on the server is unchanged, which is what matters given its size ceiling; and markdownlint's report against `main` is byte-identical, so no new lint errors. Code diff proof: no changed line under `src/` or `include/` that is not a comment.
<!-- SECTION:FINAL_SUMMARY:END -->
