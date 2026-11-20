# grep, sed, head, tail, wc, sort and uniq: design notes

The source for these six builtins keeps comments to one line each. This page holds the rationale that did not fit on one line, grouped by tool; each entry names the file and symbol it belongs to.

## grep

### `include/sh_grep.h`: SH_GREP_STEP_LIMIT

A Thompson-style simulation costs O(bytes x live states) with no backtracking, so the budget must scale with the line: a full 4096-byte line running `.*` keeps a few hundred states live and needs about two million expansions, more than any constant would allow yet no real work on the server thread. The allowance is therefore a fixed number of expansions per byte with a floor so a short line still gets a real budget. It still catches patterns whose live set grows faster than the line allows (nested unbounded repeats, long chains of starred groups, the shapes that make a backtracker exponential); those are refused with SH_GREP_STEP_MAX and errno EAGAIN because a wrong answer is worse than no answer and nothing may park the server's one poll loop. The allowance is sized from measurements at the device's line limit: ordinary BREs (`.*syslog`, `^x*$`, alternations) finish a full-length line inside it and `\(a*\)*\(a*\)*b` does not.

### `include/sh_grep.h`: sh_grep_* naming

The Vita SDK's regex.h declares regcomp but no Vita library defines it, so these are named sh_grep_* to avoid colliding with a libc that does. The SDK header cannot be included here because it needs off_t without <sys/types.h>.

### `src/sh_grep.c`: matcher (Thompson simulation)

The matcher advances the set of (node, position-in-a-repeat) pairs together at each byte and adds a pair reached twice only once, so no alternative is explored twice and cost is the line times the nodes reachable at a position, bounded by the program and never by the shape of the pattern. That matters because it runs in the server's one poll loop, where a pattern whose cost depends on its own shape, such as `\(a*\)*b` over a few hundred bytes, would hold every session's socket the way a stalled socket write does. Walking the tree rather than a pre-flattened state graph keeps the compiler honest: a node's continuation is passed in when it is reached, so a CAT, a group and a repeat each say where their tail goes without a patch-up pass that could leave a hole. SH_GREP_STEP_LIMIT is the backstop, charged once per node visited, and an outgrown pattern answers EAGAIN rather than guessing "no match". The walk is iterative over an explicit pending list, not recursive, because it runs on the server thread whose stack is the fixed SERVER_STACK given to sceKernelCreateThread.

### `src/sh_grep.c`: live-state key encoding

A live state is a node plus how far through a repeat it is, tagged with the position so that advancing is one increment rather than a clear. The fields are laid out so no two distinct states share a key and 0 alone means "slot unused"; a bias into the count's low bit would make copies 0 and 1 of a repeat indistinguishable, which is exactly how `a\{2\}` would come to accept a single character. PROGRAM_MAX nodes and counts to 255 take 10 and 8 bits, and the node index starts at bit 9 so that count 0 with node 0 still leaves the whole word zero.

### `src/sh_grep.c`: copy-count clamping

A count is a property of a repeat and rides through the nodes between the repeat and the byte that ends a copy, so a consume node must keep the count it is handed or `a\{2\}` can never finish its second copy. Only an unbounded repeat's own count is reduced: past its minimum one copy and a hundred are the same state, and clamping there is what keeps a nested pair of stars (`\(a*\)*`) from holding a live state per (inner, outer) copy-count pair, which is quadratic in the line and exhausts the budget on a no-match that every real grep answers at once. The walk also visits only live states rather than every (node, count) pair, since a repeat has 256 possible counts per node and only a few are ever reachable.

### `src/sh_grep.c`: CAT seeding of the right operand

The left operand is what the pattern is trying now, but the right one is also live whenever the left can match nothing, which is exactly when the left's subtree contains a repeat whose minimum is zero, because then the pattern is already at the right operand at this same position. Seeding only the left is the classic mistake that makes `se*` refuse `e`.

### `src/sh_grep.c`: restart seeding at every position

A search may begin at any byte so the pattern's entry is live at every position, and a leading ^ is what takes this away. The restart is appended to the same seed list so the whole position is closed at once; closing the byte-consumed seeds first and the restart second lets a repeat's body be reached by both routes and consume the same byte twice.

### `src/sh_grep.c`: case folding in ranges

With -i the range test runs on folded bytes instead of storing each range twice, so a range typed in either case matches the other and a negated set stays the complement of exactly the set that was written. The folded endpoints bound a small extra span of punctuation, so both a byte and its counterpart are tested explicitly and only those two are accepted; the counterpart is added as its own range rather than a widened one because widening would swallow the punctuation between the two halves of ASCII.

### `src/sshd_builtins.c`: grep multi-file errors

With several files, an unreadable file makes grep exit 2 no matter what another file matches: an error outranks a match, because a partial result the user cannot tell is incomplete from a complete one would be the worse answer.

### `src/sh_grep.c`: sh_grep_span

sed needs where a match is, which the yes-or-no walk never tracked. `sh_grep_span()` reuses the same walk in a second mode: it starts a match at one offset only (no reseeding at every byte), records the position of each hit, and stops when no consuming state is left, so the last hit is the longest end. The leftmost start that hits wins. Anchors are judged against the whole line, so `^` cannot match at a later offset. The two degenerate anchors `^` and `$` are not "every line" for a span: they are empty at the start and at the end, which is what `s/^/> /` and `s/$/ </` rely on. Checked against a brute-force model over every `{a,b}` line up to five bytes.

### `src/sh_grep.c`: `^$` and `^X$` on an empty line

Both used to match an empty line wrongly, found while writing the span model. `^$` was compiled as "matches all" (so `grep -v '^$'` kept nothing and `grep '^$'` printed every line), and in `^a$` the CAT's right operand (the `$`) was started at once because `node_can_be_empty()` called a BOL node empty without asking about its body, so a pattern that needs a byte matched `""`. `^$` is now its own case (`blank_only`) and a BOL is empty only when its body is.

## sed

### `src/sh_sed.c`: the pure core

Parsing and applying a program carry no I/O so the host test can run them: `sh_sed_add()` turns script text into at most 32 commands, atomically (a failed script leaves the program as it was), and `sh_sed_apply()` runs one line through them, handing each printed line to a callback. `cmd_sed()` in `src/sshd_builtins.c` is only options, files and the sink.

### `src/sh_sed.c`: what is refused, and why it is not guessed

The engine has no capture groups, so `\1` in a replacement is refused instead of being quietly treated as a literal `1`. `\n` and `\t` are refused for the same reason: GNU reads them as a newline and a tab, and passing the letter through would be a silent difference. The `s` flags `p`, `i`, a number and `w` are refused, not ignored. A refusal quotes the start of the command it stopped at, so `1,5d` says which command was a line address.

### `src/sh_sed.c`: empty matches

`s/x*/-/g` on `abc` is `-a-b-c-`, and `s/a*/x/g` on `baaac` is `xbxcx`: an empty match directly after the previous match is skipped (one byte is copied and the walk goes on), and an empty match advances by one byte. This is GNU's rule, pinned against `gsed` by `scripts/test_sh_sed.py`.

### `src/sshd_builtins.c`: sed stops on a bad line

grep skips an over-long line and carries on because a skipped line there only shortens a report. For sed the output is a replacement for the input, so a skipped line would hand back a shorter file with an exit status the caller may not check. A too-long line, or a substitution result over `SH_SED_SPACE_MAX`, therefore stops the run with the line number. Options are parsed and every script is compiled before any input is opened, so a refused script or `-i` writes and reads nothing.

## head and tail

### `src/sh_headtail.c`: sh_ht_move sink and check_line

The sink exists because a tail -n 1000000 answer can be megabytes and the server has one thread with every other session waiting on it, so out needs only SH_HT_CHUNK bytes and no answer size is promised to the heap. check_line watches the SH_HT_LINE_MAX run as bytes arrive rather than afterwards, because a chunk handed to the sink cannot be withdrawn: a line over the bound has to be caught before the chunk carrying it goes out. Returns the bytes produced, or -1 with errno set.

### `src/sh_headtail.c`: tail line walk bounds

end is file_size, or file_size - 1 when the last byte is a newline, because that newline ends the last line rather than opening a blank one. The walk goes from end - 1 down to 0 and the answer starts one byte after the want'th newline it meets, running to file_size so the answer keeps its own trailing newline; with fewer than want newlines the start stays 0. Bytes are fetched in chunks, which changes only where a byte comes from: pos starts at end, each chunk is [from, pos) with from < pos, and pos becomes from after every pass, so pos strictly decreases and the walk cannot repeat a byte or spin.

### `include/sh_headtail.h`: sh_ht_head_lines and sh_ht_tail_lines contracts

head_lines returns E2BIG when a line exceeds SH_HT_LINE_MAX after draining the stream to the next newline so a caller reading on gets a clean line, ENAMETOOLONG when the window needs more than out_cap and no sink was given, or the read's errno; tail_lines additionally returns ERANGE when file_size is beyond SH_HT_SIZE_MAX. tail_lines reads backwards in SH_HT_CHUNK steps and stops once want lines are held, so bytes read never exceed the answer plus one chunk. With a sink the answer leaves a chunk at a time, out needs only SH_HT_CHUNK bytes, and every line is checked before its chunk goes out, so a wide line is refused rather than printed in part.

### `scripts/test_sh_headtail.c`: file-scope scratch buffers

Each buffer is as wide as the widest answer any case can ask for: a whole chunk-boundary file, or one line at SH_HT_LINE_MAX. ASan keeps small frames, so an array this size inside a function is a stack overflow rather than a fault in the code under test, and separate buffers keep one call's answer from hiding another's.

### `src/sshd_builtins.c`: ht_src_t offset reads

head and tail walk backwards, so they need reads at an absolute offset, which the sequential sh_target_read cannot give. The fd is re-seeked per call so a pseudo file, which has no real seek of its own, is served by the restart-and-skip path it uses everywhere else. With no fd the source is the session's stdin, which cannot be seeked, so it is pulled into a bounded buffer first and the walk runs on that copy.

### `src/sshd_builtins.c`: HT_STREAM_MAX

A stream has no size to ask, so it is held only up to this bound: generous next to a terminal's patience, and bounded because an unbounded pull would eat the heap that every other session needs.

### `src/sshd_builtins.c`: ht_parse_count

A count operand that is not a non-negative decimal is a usage error rather than a silent zero, because a count that prints nothing looks exactly like an empty file and a typo would be invisible.

### `src/sshd_builtins.c`: tail -f unsupported

Following a file means waiting for it to grow, and waiting is the one thing a command in this server's single poll loop cannot do: the sleep would park every other session with it, so the option is refused rather than pretended.

### `src/sshd_builtins.c`: head/tail and sort/uniq batching

Output goes out a chunk at a time through a sink so a command never holds more than SH_HT_CHUNK bytes while every other session waits on this thread. Sort and uniq hold their input whole because a walk may not park the poll loop reading the socket a byte at a time, and both bounds are enforced by the tool, which refuses an oversized input rather than answering short.

## wc, sort and uniq

### `src/sh_textops.c`: next byte and eof

The next-byte reader returns its verdict (byte, end of input, or error) separately from the byte value because a NUL is a legitimate byte and cannot double as a sentinel. End of input is only known once a read has come back empty, so the reader records that in eof instead of guessing from a short read.

### `src/sh_textops.c`: merge pass stability

The merge sort takes the left run whenever two lines compare equal, which keeps the sort stable. Stability is load-bearing for -u: it is what lets the first of a set of equal lines in input order survive.

### `src/sh_textops.c`: -u key grouping

-u is a verdict on the key alone, with no byte tie-break, and it is transitive: all lines sharing a key form one group no matter where the tie-break placed them, and the earliest line of the group in input order is the one kept. Grouping by key first and then taking each group's earliest line is what makes 1, 01 and 1.0 one line while still printing the survivor at the position the key comparison ordered it. The whole group is marked, including the survivor, so a survivor sitting earlier in sorted order is not emitted a second time when the walk reaches it.

### `src/sh_textops.c`: whole-input key digit scan

A key with more digits than the bound is refused before any comparison is trusted, and the whole input is scanned for it rather than only the pairs a merge happens to compare, because two lines that are never compared against each other must still fail the run.

### `scripts/test_sh_textops.c`: reference strategy

The references are naive line loops written from the specification, not from the implementation, so agreement between the two says something. The Python wrapper pins those references against a handful of real gwc/gsort/guniq calls before running this binary so a wrong reference is caught rather than copied. Everything is in memory: the exhaustive part is every string up to length 6 over {a, b, newline, space, 1, -} for every flag combination, with no subprocess per case.

## Shared by the builtins

### `src/sshd_builtins.c`: ht_slurp_stdin and to_load_stdin refusal

The tools need random access and the stdin queue offers none, so stdin is slurped into one bounded buffer. Over the bound the input is refused rather than truncated, because a silently short head (or sort/uniq answer) is a wrong answer, not a partial one.
