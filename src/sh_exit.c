#include "sh_exit.h"

#include <limits.h>

sh_exit_verdict_t sh_exit_parse(int argc, char **argv, int last_status, int *status)
{
    const char *p;
    long value = 0;

    if (argc <= 1) {
        *status = last_status & 0xff;
        return SH_EXIT_END;
    }
    p = argv[1];
    if (*p < '0' || *p > '9') {
        *status = 2;
        return SH_EXIT_BAD_NUMBER;
    }
    for (; *p != '\0'; p++) {
        if (*p < '0' || *p > '9' || value > (INT_MAX - (*p - '0')) / 10) {
            *status = 2;
            return SH_EXIT_BAD_NUMBER;
        }
        value = value * 10 + (*p - '0');
    }
    if (argc > 2) {
        *status = 1;
        return SH_EXIT_TOO_MANY;
    }
    *status = (int)(value & 0xff);
    return SH_EXIT_END;
}
