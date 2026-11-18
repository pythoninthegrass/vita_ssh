#include "sh_lineq.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

void sh_lineq_init(sh_lineq_t *q)
{
    memset(q, 0, sizeof(*q));
}

void sh_lineq_clear(sh_lineq_t *q)
{
    int i;

    for (i = 0; i < q->len; i++) {
        free(q->slot[(q->head + i) % SH_LINEQ_MAX]);
    }
    sh_lineq_init(q);
}

int sh_lineq_len(const sh_lineq_t *q)
{
    return q->len;
}

int sh_lineq_push(sh_lineq_t *q, const char *line)
{
    char *copy;

    if (q == NULL || line == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (q->len == SH_LINEQ_MAX) {
        errno = ENOSPC;
        return -1;
    }
    copy = strdup(line);
    if (copy == NULL) {
        errno = ENOMEM;
        return -1;
    }
    q->slot[(q->head + q->len) % SH_LINEQ_MAX] = copy;
    q->len++;
    return 0;
}

int sh_lineq_pop(sh_lineq_t *q, char *buf, size_t buflen)
{
    char *line;
    size_t n;

    if (q == NULL || buf == NULL || buflen == 0) {
        errno = EINVAL;
        return -1;
    }
    if (q->len == 0) {
        errno = ENOENT;
        return -1;
    }
    line = q->slot[q->head];
    n = strlen(line);
    if (n >= buflen) {
        errno = ENAMETOOLONG;
        return -1;
    }
    memcpy(buf, line, n + 1);
    free(line);
    q->slot[q->head] = NULL;
    q->head = (q->head + 1) % SH_LINEQ_MAX;
    q->len--;
    return 0;
}
