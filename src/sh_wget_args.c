/* wget's command line for the shared curl engine; see include/sh_wget_args.h. */
#include "sh_wget_args.h"

#include <stdio.h>
#include <string.h>

static int refuse(char *msg, size_t cap, const char *opt, const char *what)
{
    snprintf(msg, cap, "%s: %s is not supported (this is a fetcher, not a crawler)", opt, what);
    return -1;
}

/* A long option: 0 handled, -1 refused or unknown (msg set); *next_used is 1 when it took argv[i + 1]. */
static int long_option(sh_wget_args_t *a, int argc, char **argv, int i, int *next_used, char *msg, size_t cap)
{
    const char *o = argv[i];

    if (strcmp(o, "--quiet") == 0) {
        a->quiet = 1;
    } else if (strcmp(o, "--no-check-certificate") == 0) {
        a->insecure = 1;
    } else if (strcmp(o, "--content-disposition") == 0) {
        a->use_dispo = 1;
    } else if (strcmp(o, "--recursive") == 0) {
        return refuse(msg, cap, o, "recursive download");
    } else if (strcmp(o, "--page-requisites") == 0) {
        return refuse(msg, cap, o, "fetching page requisites");
    } else if (strcmp(o, "--mirror") == 0) {
        return refuse(msg, cap, o, "mirroring");
    } else if (strcmp(o, "--output-document") == 0 || strncmp(o, "--output-document=", 18) == 0) {
        const char *v = o[17] == '=' ? o + 18 : (i + 1 < argc ? argv[i + 1] : NULL);

        if (v == NULL || v[0] == '\0') {
            snprintf(msg, cap, "%s needs a file name", "--output-document");
            return -1;
        }
        if (o[17] != '=')
            *next_used = 1;
        a->out_file = strcmp(v, "-") == 0 ? NULL : v;
        a->to_terminal = strcmp(v, "-") == 0;
    } else {
        snprintf(msg, cap, "%s: unknown option", o);
        return -1;
    }
    return 0;
}

int sh_wget_parse(int argc, char **argv, sh_wget_args_t *a, char *msg, size_t cap)
{
    int i, j, flags_done = 0;

    memset(a, 0, sizeof(*a));
    if (cap > 0)
        msg[0] = '\0';
    for (i = 1; i < argc; i++) {
        const char *o = argv[i];

        if (flags_done || o[0] != '-' || o[1] == '\0') {
            if (a->url != NULL) {
                snprintf(msg, cap, "only one URL is supported");
                return -1;
            }
            a->url = o;
            continue;
        }
        if (strcmp(o, "--") == 0) {
            flags_done = 1;
            continue;
        }
        if (o[1] == '-') {
            int used = 0;

            if (long_option(a, argc, argv, i, &used, msg, cap) != 0)
                return -1;
            i += used;
            continue;
        }
        for (j = 1; o[j] != '\0'; j++) {
            switch (o[j]) {
            case 'q':
                a->quiet = 1;
                break;
            case 'r':
                return refuse(msg, cap, "-r", "recursive download");
            case 'p':
                return refuse(msg, cap, "-p", "fetching page requisites");
            case 'm':
                return refuse(msg, cap, "-m", "mirroring");
            case 'O': {
                const char *v = o[j + 1] != '\0' ? o + j + 1 : (i + 1 < argc ? argv[++i] : NULL);

                if (v == NULL || v[0] == '\0') {
                    snprintf(msg, cap, "-O needs a file name");
                    return -1;
                }
                a->out_file = strcmp(v, "-") == 0 ? NULL : v;
                a->to_terminal = strcmp(v, "-") == 0;
                j = (int)strlen(o) - 1;
                break;
            }
            default:
                snprintf(msg, cap, "-%c: unknown option", o[j]);
                return -1;
            }
        }
    }
    return a->url == NULL ? 1 : 0;
}
