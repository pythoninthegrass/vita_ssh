#include "sh_listen.h"

#include <string.h>

void sh_listen_init(sh_listen_t *t)
{
    memset(t, 0, sizeof(*t));
}

void sh_listen_failed(sh_listen_t *t, unsigned long long now_us)
{
    t->down = 1;
    t->retry_at_us = now_us + t->backoff_us;
    if (t->backoff_us == 0) {
        t->backoff_us = SH_LISTEN_BACKOFF_FIRST_US;
    } else if (t->backoff_us < SH_LISTEN_BACKOFF_MAX_US) {
        t->backoff_us *= 2;
        if (t->backoff_us > SH_LISTEN_BACKOFF_MAX_US) {
            t->backoff_us = SH_LISTEN_BACKOFF_MAX_US;
        }
    }
}

int sh_listen_due(const sh_listen_t *t, unsigned long long now_us)
{
    return t->down && now_us >= t->retry_at_us;
}

int sh_listen_is_down(const sh_listen_t *t)
{
    return t->down;
}

void sh_listen_rebuilt(sh_listen_t *t)
{
    t->down = 0;
}

void sh_listen_accepted(sh_listen_t *t)
{
    t->backoff_us = 0;
}
