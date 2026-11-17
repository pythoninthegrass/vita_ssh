/* Line matching for grep: a bounded POSIX basic engine, no I/O and no Vita APIs,
 * so the same code the server links is what the host check exercises. */
#ifndef SH_GREP_H
#define SH_GREP_H

#include <stddef.h>

/* Longest line grep accepts. A longer one is reported as over the limit rather
 * than matched, because a truncated line would answer a question nobody asked. */
#define SH_GREP_LINE_MAX 4096

/* Longest pattern grep accepts. */
#define SH_GREP_PATTERN_MAX 256

/* Work budget for one match attempt, in node expansions.
 *
 * A Thompson-style simulation costs O(bytes x live states) with no backtracking, so the
 * budget has to scale with the line: a full 4096-byte line that runs `.*` keeps a few
 * hundred states live and needs about two million expansions, which is no work at all on
 * the server thread but is more than any constant would allow. The allowance is therefore
 * a fixed number of expansions per byte rather than a flat figure, with a floor so a short
 * line still gets a real budget.
 *
 * What the budget still catches is a pattern whose live set grows faster than the line
 * allows -- nested unbounded repeats and long chains of starred groups, the shapes that
 * make a backtracker exponential. Those charge far more per byte than the allowance and
 * are refused -- match returns SH_GREP_STEP_MAX and sets errno EAGAIN -- because a wrong
 * answer is worse than no answer and nothing may park the server's one poll loop. The
 * allowance is sized from measurements on the device's own line limit: ordinary BREs
 * (`.*syslog`, `^x*$`, alternations) finish a full-length line inside it, and
 * `\(a*\)*\(a*\)*b` does not. */
#define SH_GREP_STEP_PER_BYTE 1024
#define SH_GREP_STEP_FLOOR 200000
#define SH_GREP_STEP_LIMIT(len)                                            \
    ((long)(len) * SH_GREP_STEP_PER_BYTE > SH_GREP_STEP_FLOOR               \
         ? (long)(len) * SH_GREP_STEP_PER_BYTE : SH_GREP_STEP_FLOOR)

/* match returns this, with errno EAGAIN, when the attempt outruns SH_GREP_STEP_LIMIT. */
#define SH_GREP_STEP_MAX (-1)

/* The pattern syntax grep implements, for the usage string and README. */
#define SH_GREP_SYNTAX "POSIX basic regular expressions: . [ ] ^ $ \\( \\) \\{m,n\\} \\| \\+ \\?"

/* regcomp is declared by the Vita SDK's regex.h but no Vita library defines it, so
 * these are named sh_grep_* rather than regcomp to avoid colliding with a libc that
 * does. The SDK header cannot be included here: it needs off_t without <sys/types.h>. */
#define SH_GREP_ICASE 0x0001 /* compare ASCII case-insensitively */

typedef struct sh_grep_re sh_grep_t;

/* Compiles pattern. Returns NULL with errno EINVAL for a malformed pattern or
 * ENOMEM, or ERANGE when it needs more program than SH_GREP_PROGRAM_MAX nodes. */
sh_grep_t *sh_grep_compile(const char *pattern, int flags);
void sh_grep_free(sh_grep_t *re);

/* One line against one compiled pattern, honouring -v. Returns 1 when the line
 * should be printed, 0 when it should not, and -1 with errno EAGAIN when the
 * match hit SH_GREP_STEP_MAX -- no answer is better than a wrong one. */
int sh_grep_match(const sh_grep_t *re, const char *line, int invert);

/* Why sh_grep_compile failed, for the error line. Valid after a NULL return. */
const char *sh_grep_compile_error(char *dst, size_t dstlen);

/* Byte-wise substring search, ASCII case-folded when icase. Returns the start of
 * the first occurrence, or NULL. */
const char *sh_grep_find(const char *haystack, const char *needle, size_t needle_len,
                         int icase);

/* Folds ASCII case; bytes outside A-Z a-z come back unchanged. */
char sh_grep_fold(int c);

/* A byte source for sh_grep_read_line: copies up to want bytes into p, returns how
 * many it took (0 at end of input). */
typedef size_t (*sh_grep_fill_fn)(void *ctx, char *p, size_t want);

/* Reads one line through fill, without its newline. Returns the length, 0 at end of
 * input, or -1 when the line is longer than cap (SH_GREP_LINE_MAX is the bound grep
 * uses). An overlong line is drained to the next newline before the -1, so the caller
 * sees one verdict per long line and the next read starts a fresh line. */
int sh_grep_read_line(sh_grep_fill_fn fill, void *ctx, char *line, size_t cap,
                      int *eof);

#endif
