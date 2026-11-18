/* Chaining a line's commands with ; && and ||: the operators, the syntax check and the run-or-skip rule, with no I/O and no Vita APIs. */
#ifndef SH_CHAIN_H
#define SH_CHAIN_H

/* Most commands one line may chain; a longer line is refused whole, so one line cannot monopolise the poll loop. */
#define SH_CHAIN_MAX 16

typedef enum {
    SH_CHAIN_NONE = 0, /* not an operator */
    SH_CHAIN_SEQ,      /* ;  always run the next command */
    SH_CHAIN_AND,      /* && run the next command only after status 0 */
    SH_CHAIN_OR        /* || run the next command only after a nonzero status */
} sh_chain_op_t;

/* sh_chain_check results besides an item count. */
#define SH_CHAIN_SYNTAX (-1)   /* leading, doubled or trailing operator */
#define SH_CHAIN_TOO_LONG (-2) /* more than SH_CHAIN_MAX commands */

/* An operator is an unquoted word that is exactly ; && or ||, so `a;b` and "&&" stay text, as for `|` and `>`. */
sh_chain_op_t sh_chain_op(const char *word, int quoted);

/* The operator's spelling, for the error line. */
const char *sh_chain_op_name(sh_chain_op_t op);

/* Validates a whole line before anything runs. Returns the number of commands (1 when there is no operator),
 * SH_CHAIN_SYNTAX with *bad set to the offending operator, or SH_CHAIN_TOO_LONG. bad may be NULL. As in sh,
 * one trailing ; is accepted and starts no command; a leading, doubled or trailing && or || is an error. */
int sh_chain_check(char **argv, const int *quoted, int argc, sh_chain_op_t *bad);

/* The words that are commands once a valid line's single trailing ; is dropped: argc, or argc - 1. */
int sh_chain_words(char **argv, const int *quoted, int argc);

/* Whether the command after op runs, given the status of the last command that ran. */
int sh_chain_should_run(sh_chain_op_t op, int status);

#endif
