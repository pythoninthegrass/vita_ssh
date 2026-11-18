/* A bounded FIFO of completed input lines: no I/O and no Vita APIs. */
#ifndef SH_LINEQ_H
#define SH_LINEQ_H

#include <stddef.h>

/* Most lines held at once. Each is a heap copy of its exact length, so an idle queue costs the
 * slot pointers alone. */
#define SH_LINEQ_MAX 8

typedef struct {
    char *slot[SH_LINEQ_MAX];
    int head;
    int len;
} sh_lineq_t;

void sh_lineq_init(sh_lineq_t *q);

/* Frees every held line. */
void sh_lineq_clear(sh_lineq_t *q);

int sh_lineq_len(const sh_lineq_t *q);

/* Appends a copy of line. Returns 0, or -1 with errno: EINVAL (NULL queue or line), ENOSPC (queue
 * full; nothing held is touched), ENOMEM. */
int sh_lineq_push(sh_lineq_t *q, const char *line);

/* Copies the oldest line into buf and removes it. Returns 0, or -1 with errno: EINVAL (NULL queue,
 * NULL buf or buflen 0), ENOENT (queue empty), ENAMETOOLONG (buf too small; the line stays). */
int sh_lineq_pop(sh_lineq_t *q, char *buf, size_t buflen);

#endif
