/* The argument rules of the `exit` builtin: no I/O and no Vita APIs. */
#ifndef SH_EXIT_H
#define SH_EXIT_H

typedef enum {
    SH_EXIT_END,       /* end the session with *status */
    SH_EXIT_BAD_NUMBER, /* argv[1] is not a status: report it, then end the session with *status (2) */
    SH_EXIT_TOO_MANY,  /* more than one argument: report it, keep the session, return *status (1) */
} sh_exit_verdict_t;

/* argv[0] is the command name. No argument gives last_status; a status is digits only and is reduced
 * modulo 256, and a sign, a space, text or a value too big for an int is SH_EXIT_BAD_NUMBER. A bad
 * first argument wins over extra arguments, as in bash. */
sh_exit_verdict_t sh_exit_parse(int argc, char **argv, int last_status, int *status);

#endif
