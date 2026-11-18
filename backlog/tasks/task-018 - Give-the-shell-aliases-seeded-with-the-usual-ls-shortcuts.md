---
id: TASK-018
title: 'Give the shell aliases, seeded with the usual ls shortcuts'
status: Done
assignee:
  - Claude
created_date: '2026-11-17 03:28'
updated_date: '2026-11-18T07:38:00Z'
labels:
  - shell
dependencies:
  - TASK-025
documentation:
  - AGENTS.md
modified_files:
  - src/sh_alias.c
  - include/sh_alias.h
  - scripts/test_sh_alias.c
  - scripts/test_sh_alias.py
  - src/sshd_builtins.c
  - src/sshd_session.c
  - include/sshd.h
  - CMakeLists.txt
  - README.md
  - AGENTS.md
  - scripts/smoke.sh
priority: low
ordinal: 32000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
Common boxes ship l and ll; the shell has neither. Wanted: an alias/unalias builtin over per-session aliases, seeded at session setup with the ls shortcuts people expect.

Aliases are the only short-name mechanism available here: the Vita has no symlinks and the platform rules forbid faking one (readlink and symlink answer ENOSYS on purpose), so there is nothing to ln -s a directory with.

Expansion design: expansion happens in command position only, after word-splitting and before redirect extraction -- so l > file still redirects, and grep ll does not expand. Scope expanded by the user on 2026-11-18 to follow bash: an alias whose value starts with another alias expands through it, each alias is applied at most once per command (so alias ls='ls -l' reaches the builtin ls), and a definition that would close a ring of aliases is refused, because a hung poll loop takes every session with it.

State lives with the session (like cwd does in sh_fsctx_t), never in libc statics: two people logged in must not see each other's aliases.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [x] #1 alias lists the current name=value pairs; alias name=value defines one; unalias name removes one and reports unalias: name: not found with a nonzero exit for a name that was never defined.
- [x] #2 A fresh session can run l, ll and la for ls -l, ls -la and ls -a, and .. and ... move up one and two directories.
- [x] #3 Aliases expand only in command position: grep ll searches for the word ll, and an argument that happens to name an alias is untouched.
- [x] #4 An alias cycle such as alias l=ll with alias ll=l is refused or broken with a readable message and never hangs the session.
- [x] #5 A value with spaces survives quoting: alias two=echo a b then two prints a b.
- [x] #6 l > /ux0/data/x writes the listing to that file: expansion happens before redirect extraction.
- [x] #7 Aliases are per session and die with it; a second session never sees them.
- [x] #8 history recalls the line as the user typed it, not the expanded form.
- [x] #9 A user alias shadows a builtin of the same name, and help says which one wins.
- [x] #10 An alias whose value starts with another alias expands through it, as in bash: with ll defined as ls -la, alias l=ll makes l run ls -la; each alias is expanded at most once per command, so alias ls='ls -l' reaches the builtin ls and an alias name that comes back around is left as a plain command word.
- [x] #11 A definition that would close a loop of aliases (alias ll=l after alias l=ll, or a longer ring) is refused with a readable message and leaves the old value, so no loop can ever be defined; naming itself (alias ls='ls -l') is not a loop.
<!-- AC:END -->

## Definition of Done
<!-- DOD:BEGIN -->
- [x] #1 task build produces both VPKs
- [x] #2 Verified on a device over a real ssh session
<!-- DOD:END -->

## Implementation Plan

<!-- SECTION:PLAN:BEGIN -->
Refreshed 2026-11-18 after TASK-025 shipped; the older plan's line numbers had moved. Same approach the user chose for TASK-025: the pure logic in a module with a host test written first (TDD), the glue checked on the device.

Current code: sh_execute_line() checks the chain, then run_item() or run_chain() hand each item to run_pipeline()/run_command(); run_command() takes one command's words (MAX_WORDS arrays, owns and frees them) and extracts redirects before globbing, so every chain item and every pipeline segment has its own command position there. split_line() (static in sshd_builtins.c) gives words plus one quoted bit each. The session struct is `struct sh_session` in src/sshd_session.c with accessors in include/sshd.h.

1. Pure module include/sh_alias.h and src/sh_alias.c, no Vita APIs: a heap table (SH_ALIAS_MAX 32 entries, names up to 32 bytes from an allow-list of letters, digits and `_ . - + , : @ % ^`, values up to 256 bytes, kept sorted by name), sh_alias_set / unset / get / count / at, sh_alias_seed (l, ll, la, `..`, `...`), and the definition rule: a value whose first word names ANOTHER alias is refused (ELOOP) because aliases expand one level and that alias would never be followed; naming itself is allowed, as in sh (`alias ls='ls -l'`). That refusal is what AC4 asks for: no cycle can be defined, and none could hang the loop anyway.
2. Host test first: scripts/test_sh_alias.c and test_sh_alias.py under ASan/UBSan, run under timeout: fixed cases from the ACs plus every operation sequence up to length 4 over three names and five values compared against a naive model; confirm it fails before the module exists.
3. Session: struct sh_session owns a sh_alias_table_t, created and seeded in sh_session_new(), freed in sh_session_free(); sh_session_aliases(s) in include/sshd.h. Never a static, so two sessions never share (AC7).
4. Expansion at the top of run_command(), before extract_redirect (AC6): when argv[0] is unquoted and names an alias, split the value with split_line(), splice its words in front of argv[1..], keeping each word's quoted bit, and do not look at the first word again. Chain and pipe operators inside a value arrive as plain words because the line is already split. Room check against MAX_WORDS with a readable error.
5. Builtins alias (list sorted as `alias name='value'`, `alias name` shows one, `alias name=value` defines, several per call, `alias: NAME: not found`) and unalias (`unalias: NAME: not found`, status 1), rows alphabetical. A user alias shadows a builtin because the alias is looked up first; help's footer and which say so (AC9).
6. history keeps the typed line already; confirm AC8 on a device.
7. smoke.sh exec checks, README, help; AGENTS.md host test and server list; arm-vita-eabi-size before (2298476 at e10607e) and after.
8. Verify on the device over ssh and an interactive session; install through VitaShell over USB.

Risks: size (server text rose 1.9 KB this week; the ceiling headroom is unmeasured, so watch the link); an alias naming another alias is refused rather than followed, which differs from bash and is documented; a value containing `>` redirects like the text it stands for, as in sh. TASK-020 expands variables after aliases, so keep the expansion point as one function it can follow.

Scope expanded by the user on 2026-11-18: aliases follow bash and expand through other aliases instead of refusing them. This replaces the earlier rule that a value starting with another alias is refused (step 1) and the one-level expansion (step 4). New rules: the expansion applies an alias, then looks at the first word of the result, and repeats; an alias already applied in this command is not applied again (so alias ls='ls -l' reaches the builtin), which bounds the walk at the table size. A definition that would make a ring of two or more aliases is still refused with a readable message (ELOOP, AC11), a stricter choice than bash, which allows it and fails at run time, and a loop can therefore never be defined. Bash's rule that an alias ending in a blank also expands the next word is NOT included; ask before adding it. Pure part: sh_alias_chain(t, word, out, max) returns the values to apply in order, and the ring check is the same walk, both under the host test, written first. The glue applies the values one at a time with the existing splice. README and help change from 'one level' to 'follows other aliases, each at most once'.

Final solution differs from steps 1 and 4 as written: aliases follow bash (expand through other aliases, each at most once) on the user's decision of 2026-11-18, and only a definition that closes a ring is refused. The expansion lives in expand_alias() and splice_alias_value() at the top of run_command(); sh_alias_chain() in the pure module decides the order.
<!-- SECTION:PLAN:END -->

## Implementation Notes

<!-- SECTION:NOTES:BEGIN -->
2026-11-18: implemented per the plan. include/sh_alias.h and src/sh_alias.c are the pure table (SH_ALIAS_MAX 32, names to 32 bytes from letters, digits and _ . - + , : @ % ^, values to 256, sorted listing, a value whose first word is another alias refused with ELOOP, its own name allowed, blank values refused); scripts/test_sh_alias.c and .py were written first and failed to build without the module. Mutation check: six broken versions each fail it, and one that survived (a boundary on the longest name) got a test, then failed too. Two bits of dead code the mutants exposed (quote checks on the first word, which can never equal a name) were removed. The table lives in struct sh_session (sh_session_aliases(), created and seeded in sh_session_new(), freed in sh_session_free()); expand_alias() runs at the top of run_command() before redirects are extracted, so every pipeline segment and chain item has its own command position; the value is split with split_line() and the result is not looked up again. alias and unalias builtins, which and help report an alias and say it wins over a builtin; README, AGENTS.md and scripts/smoke.sh exec checks updated. Host tests sh_alias, sh_chain, sh_grep, sh_headtail, sh_textops pass. Server text 2301000 against 2298476 (+2524). Not yet run on a device.

2026-11-18 device run (the one Vita, home network). The build was installed with `vpk install` over ssh this time (scp -s, then vpk install): result.txt reads 'promote package: 0x00000000', the server came back on its own and answered `help ll` and `alias` from the new build. scripts/smoke.sh exec: 98 pass 1 fail, then 99 pass 0 fail. The first full run had four failures: three were wrong assumptions in my checks (ls -l / prints the device table in another format, so compare `l /ux0` with `ls -l /ux0` instead; `...` clamps at the device root, so from /ux0/data/vitasshd it lands on /ux0), and one was a transient empty result for `uname -a ; pwd` that passed three times by hand. A later run had one `ls /` check fail with ssh exit 255, which passed on the next run. These intermittent connection failures (an empty result or exit 255, about one per hundred ssh calls, also seen once on TASK-025's `echo "a; b"`) have no known cause. Two interactive sessions at once: a definition made in one was not visible in the other (`hello` was command not found there, `alias` listed only the five seeded entries), and `history` in the defining session showed `alias hello='echo hi there'` and `hello` as typed, not expanded (AC7, AC8). AC9: an alias named like a builtin wins, and help says so (smoke). Spelling in the touched prose was changed to US English per the updated rules.

2026-11-18 scope expansion (user: follow bash for alias chaining). The one-level rule and the refusal of a value starting with another alias are replaced. sh_alias_chain(t, word, out, max) returns the values to apply in order, each alias at most once, so alias ls='ls -l' reaches the builtin; sh_alias_set now refuses only a definition that closes a ring (ELOOP; a value starting with the alias's own name is not a ring) and leaves the old value. Host test rewritten first and failed to link without sh_alias_chain; then it passed, including every sequence up to four operations over three names and five values compared against a model of the chain and the ring rule. Two expectation errors in my fixed cases (an earlier alias for ls lengthened a chain) were test bugs, fixed in the test. Mutation check: six broken versions (self counted as a ring, no visited set in the ring walk, chain ignores the applied set, max off by one, ring ignoring existing values, chain stopping after one step) each fail it. The glue splits the walk into splice_alias_value() and an expand_alias() that applies the chain's values one at a time. help, the alias usage and error text, README and AGENTS.md updated; bash's blank-ending alias rule is not implemented and the README says so. Server text 2301384 (+384 over the previous alias build, +2908 over the chaining build). Device: installed with vpk install over ssh (result.txt 'promote package: 0x00000000'); `alias m=ll` is accepted by the new build. scripts/smoke.sh exec: 102 pass 0 fail on the first run, covering AC10 and AC11 (a value starting with another alias expands through it, a name coming back around reaches the builtin, closing a ring exits 1 with 'loop of aliases' and stores nothing). A second run had 99 pass 3 fail, all three ssh exit 255 on checks that pass on the other run (ls /app0, traversal above root, closing a ring); same intermittent connection failures as noted above, rate higher in this run, cause still unknown. Possibly related to TASK-012.
<!-- SECTION:NOTES:END -->

## Final Summary

<!-- SECTION:FINAL_SUMMARY:BEGIN -->
Added per-session aliases. alias lists (as alias name='value'), shows one, or defines one or more; unalias removes and reports 'unalias: NAME: not found' with status 1. A new session starts with l (ls -l), ll (ls -la), la (ls -a), .. (cd ..) and ... (cd ../..). Expansion happens only for the command word of a command, pipeline segment or chain item, before redirects are read, and follows bash: an alias whose value starts with another alias expands through it, each alias at most once per command, so alias ls='ls -l' reaches the builtin. A definition that would close a ring (alias ll=l after alias l=ll) is refused with 'that value would make a loop of aliases' and leaves the old value; this is stricter than bash. A user alias wins over a builtin, and help and which say so. Aliases live in the session struct, so sessions never share them, and history shows the line as typed. The logic is the pure module src/sh_alias.c, written test-first under ASan/UBSan against a model over every operation sequence up to four, with broken versions of the module each failing the test; the glue is in src/sshd_builtins.c and src/sshd_session.c. Verified on the Vita over ssh: exec smoke 102 pass 0 fail on one run, and two concurrent interactive sessions showed isolation and as-typed history. Server text grew 2,908 bytes over the chaining build. Caveats: a later exec run had three ssh exit-255 failures on checks that pass on other runs (intermittent connection failures, cause unknown, possibly TASK-012); bash's rule that an alias ending in a blank also expands the next word is not implemented.
<!-- SECTION:FINAL_SUMMARY:END -->
