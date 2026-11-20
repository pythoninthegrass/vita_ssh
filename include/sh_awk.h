/* A small awk: one [pattern] { print ... } rule over fields of one line at a time. No I/O. */
#ifndef SH_AWK_H
#define SH_AWK_H

#include <stddef.h>

/* Fields one line may split into; a line with more is reported, not truncated. */
#define SH_AWK_FIELD_MAX 128

/* Arguments one print statement, and statements one action, may hold. */
#define SH_AWK_PRINT_ARG_MAX 16
#define SH_AWK_STMT_MAX 8

/* Longest string literal in a program. */
#define SH_AWK_STR_MAX 128

/* Longest line one print statement produces, so a long join is refused, not cut. */
#define SH_AWK_OUT_MAX 8192

/* The subset awk implements, for the usage string and README. */
#define SH_AWK_SYNTAX "[/RE/ | OPERAND == != < <= > >= OPERAND] { print [ARG, ...] } with $N $NF NF NR"

typedef struct sh_awk sh_awk_t;

sh_awk_t *sh_awk_new(void);
void sh_awk_free(sh_awk_t *p);

/* Parses program (the text of one rule) and the field separator fs (NULL for runs of blanks);
 * 0, or -1 with why filled, such as "unsupported: BEGIN", and nothing kept. */
int sh_awk_compile(sh_awk_t *p, const char *program, const char *fs, char *why, size_t whylen);

/* Receives each line a print statement produces, without its newline. */
typedef void (*sh_awk_emit_fn)(void *ctx, const char *line, size_t len);

/* Runs the rule on one line and advances NR (which carries across files, as in awk). Returns 0,
 * or -1 with errno ERANGE (more than SH_AWK_FIELD_MAX fields, or a print longer than
 * SH_AWK_OUT_MAX), EAGAIN (a regex outran the grep engine's step limit) or ENOMEM. */
int sh_awk_apply(sh_awk_t *p, const char *line, sh_awk_emit_fn emit, void *ctx);

#endif
