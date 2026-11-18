/* Chaining a line's commands with ; && and ||; see include/sh_chain.h. */
#include "sh_chain.h"

#include <string.h>

sh_chain_op_t sh_chain_op(const char *word, int quoted)
{
    if (quoted) {
        return SH_CHAIN_NONE;
    }
    if (strcmp(word, ";") == 0) {
        return SH_CHAIN_SEQ;
    }
    if (strcmp(word, "&&") == 0) {
        return SH_CHAIN_AND;
    }
    if (strcmp(word, "||") == 0) {
        return SH_CHAIN_OR;
    }
    return SH_CHAIN_NONE;
}

const char *sh_chain_op_name(sh_chain_op_t op)
{
    switch (op) {
    case SH_CHAIN_SEQ:
        return ";";
    case SH_CHAIN_AND:
        return "&&";
    case SH_CHAIN_OR:
        return "||";
    default:
        return "";
    }
}

int sh_chain_check(char **argv, const int *quoted, int argc, sh_chain_op_t *bad)
{
    int i, items = 1;

    for (i = 0; i < argc; i++) {
        sh_chain_op_t op = sh_chain_op(argv[i], quoted[i]);

        if (op == SH_CHAIN_NONE) {
            continue;
        }
        /* An operator needs a command before it, and after it unless it is a final ; as in sh. */
        if (i == 0 || sh_chain_op(argv[i - 1], quoted[i - 1]) != SH_CHAIN_NONE ||
            (i == argc - 1 && op != SH_CHAIN_SEQ)) {
            if (bad != NULL) {
                *bad = op;
            }
            return SH_CHAIN_SYNTAX;
        }
        if (i < argc - 1) {
            items++;
        }
    }
    return items > SH_CHAIN_MAX ? SH_CHAIN_TOO_LONG : items;
}

int sh_chain_words(char **argv, const int *quoted, int argc)
{
    if (argc >= 2 && sh_chain_op(argv[argc - 1], quoted[argc - 1]) == SH_CHAIN_SEQ) {
        return argc - 1;
    }
    return argc;
}

int sh_chain_should_run(sh_chain_op_t op, int status)
{
    switch (op) {
    case SH_CHAIN_AND:
        return status == 0;
    case SH_CHAIN_OR:
        return status != 0;
    default:
        return 1;
    }
}
