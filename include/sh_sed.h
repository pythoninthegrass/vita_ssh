/* A small sed: s, d, p and q over one line at a time, matched by the grep engine. No I/O. */
#ifndef SH_SED_H
#define SH_SED_H

#include <stddef.h>

/* Commands one program may hold, across every script added. */
#define SH_SED_CMD_MAX 32

/* Longest replacement text after parsing. */
#define SH_SED_REPL_MAX 256

/* Longest pattern space, so a substitution that grows a line is refused, not truncated. */
#define SH_SED_SPACE_MAX 8192

/* The subset sed implements, for the usage string and README. */
#define SH_SED_SYNTAX "s/RE/TEXT/[g] d p q, separated by ; or newline; & and \\& in TEXT"

typedef struct sh_sed sh_sed_t;

/* What sh_sed_apply tells its caller. */
#define SH_SED_NEXT 0 /* go on to the next line */
#define SH_SED_QUIT 1 /* q ran: stop reading */

sh_sed_t *sh_sed_new(void);
void sh_sed_free(sh_sed_t *p);

/* Parses script text and appends its commands; 0, or -1 with why filled and nothing appended. */
int sh_sed_add(sh_sed_t *p, const char *script, char *why, size_t whylen);

/* Receives each line the program prints, without its newline. */
typedef void (*sh_sed_emit_fn)(void *ctx, const char *line, size_t len);

/* Runs the program on one line. quiet is -n. Returns SH_SED_NEXT or SH_SED_QUIT, or -1 with
 * errno ERANGE (the pattern space outgrew SH_SED_SPACE_MAX), EAGAIN (a match outran the grep
 * engine's step limit) or ENOMEM. */
int sh_sed_apply(sh_sed_t *p, const char *line, int quiet, sh_sed_emit_fn emit, void *ctx);

#endif
