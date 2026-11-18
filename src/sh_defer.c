#include "sh_defer.h"

#include <errno.h>
#include <string.h>

void sh_defer_init(sh_defer_t *d)
{
    d->len = 0;
}

size_t sh_defer_len(const sh_defer_t *d)
{
    return d->len;
}

const char *sh_defer_data(const sh_defer_t *d)
{
    return d->buf;
}

int sh_defer_add(sh_defer_t *d, const char *data, size_t len)
{
    if (d == NULL || (data == NULL && len > 0)) {
        errno = EINVAL;
        return -1;
    }
    if (len > SH_DEFER_MAX - d->len) {
        errno = ENOSPC;
        return -1;
    }
    if (len > 0) {
        memcpy(d->buf + d->len, data, len);
        d->len += len;
    }
    return 0;
}

void sh_defer_reset(sh_defer_t *d)
{
    d->len = 0;
}
