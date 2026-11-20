/* Line matching for grep: a bounded POSIX basic engine with no I/O and no Vita APIs. */
#ifndef SH_GREP_H
#define SH_GREP_H

#include <stddef.h>

/* Longest line grep accepts; a longer one is reported as over the limit, not truncated. */
#define SH_GREP_LINE_MAX 4096

/* Longest pattern grep accepts. */
#define SH_GREP_PATTERN_MAX 256

/* Work budget for one match attempt, in node expansions, scaled per byte of the line. */
#define SH_GREP_STEP_PER_BYTE 1024
#define SH_GREP_STEP_FLOOR 200000
#define SH_GREP_STEP_LIMIT(len)                                            \
    ((long)(len) * SH_GREP_STEP_PER_BYTE > SH_GREP_STEP_FLOOR               \
         ? (long)(len) * SH_GREP_STEP_PER_BYTE : SH_GREP_STEP_FLOOR)

/* match returns this, with errno EAGAIN, when the attempt outruns SH_GREP_STEP_LIMIT. */
#define SH_GREP_STEP_MAX (-1)

/* The pattern syntax grep implements, for the usage string and README. */
#define SH_GREP_SYNTAX "POSIX basic regular expressions: . [ ] ^ $ \\( \\) \\{m,n\\} \\| \\+ \\?"

/* Named sh_grep_* because the Vita SDK declares regcomp but no Vita library defines it. */
#define SH_GREP_ICASE 0x0001 /* compare ASCII case-insensitively */

typedef struct sh_grep_re sh_grep_t;

/* Compiles pattern; NULL with errno EINVAL, ENOMEM, or ERANGE past SH_GREP_PROGRAM_MAX nodes. */
sh_grep_t *sh_grep_compile(const char *pattern, int flags);
void sh_grep_free(sh_grep_t *re);

/* Matches one line, honoring -v: 1 to print, 0 to skip, -1 with errno EAGAIN on step limit. */
int sh_grep_match(const sh_grep_t *re, const char *line, int invert);

/* Finds the leftmost-longest match starting at or after from: 1 with [*so, *eo), 0 for none, -1
 * with errno EAGAIN on the step limit. ^ and $ are judged against the whole line, not from. */
int sh_grep_span(const sh_grep_t *re, const char *line, size_t from, size_t *so, size_t *eo);

/* Why sh_grep_compile failed, for the error line. Valid after a NULL return. */
const char *sh_grep_compile_error(char *dst, size_t dstlen);

/* Byte-wise substring search, ASCII case-folded when icase; returns first match or NULL. */
const char *sh_grep_find(const char *haystack, const char *needle, size_t needle_len,
                         int icase);

/* Folds ASCII case; bytes outside A-Z a-z come back unchanged. */
char sh_grep_fold(int c);

/* Byte source for sh_grep_read_line: copies up to want bytes into p, returns count, 0 at EOF. */
typedef size_t (*sh_grep_fill_fn)(void *ctx, char *p, size_t want);

/* Reads one line without its newline: length, 0 at EOF, or -1 (line drained) if over cap. */
int sh_grep_read_line(sh_grep_fill_fn fill, void *ctx, char *line, size_t cap,
                      int *eof);

#endif
