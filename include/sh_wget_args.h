/* wget's command line, parsed into what the shared curl engine needs: no I/O and no Vita APIs, so host tests run this code. */
#ifndef SH_WGET_ARGS_H
#define SH_WGET_ARGS_H

#include <stddef.h>

typedef struct {
    const char *url;
    const char *out_file; /* -O FILE, or NULL */
    int to_terminal;      /* -O - */
    int quiet;            /* -q */
    int insecure;         /* --no-check-certificate */
    int use_dispo;        /* --content-disposition */
} sh_wget_args_t;

/* Returns 0 with *a filled in (pointing into argv), 1 when no URL was given (the caller prints
 * usage), or -1 with a sentence in msg: an unknown option, a missing -O operand, a second URL, or
 * a recursive option (-r, -p, -m) that this fetcher refuses rather than ignores. */
int sh_wget_parse(int argc, char **argv, sh_wget_args_t *a, char *msg, size_t cap);

#endif
