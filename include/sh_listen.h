/* When to replace the SSH listening socket after it fails: no I/O and no Vita APIs. */
#ifndef SH_LISTEN_H
#define SH_LISTEN_H

/* The wait before the second rebuild attempt; each later one doubles it, up to the cap. The first attempt is immediate. */
#define SH_LISTEN_BACKOFF_FIRST_US 500000ULL
#define SH_LISTEN_BACKOFF_MAX_US 30000000ULL

typedef struct {
    int down;
    unsigned long long retry_at_us;
    unsigned long long backoff_us;
} sh_listen_t;

void sh_listen_init(sh_listen_t *t);

/* The listener failed: an accept error, or a rebuild attempt that did not work. Marks it down and schedules the next attempt. */
void sh_listen_failed(sh_listen_t *t, unsigned long long now_us);

/* Nonzero when the listener is down and the time for the next attempt has come. */
int sh_listen_due(const sh_listen_t *t, unsigned long long now_us);

int sh_listen_is_down(const sh_listen_t *t);

/* A new listener is in place. The backoff stays until a connection is accepted, so a listener that dies at once is not rebuilt in a tight loop. */
void sh_listen_rebuilt(sh_listen_t *t);

/* A connection was accepted: the listener works, so the next failure is retried at once. */
void sh_listen_accepted(sh_listen_t *t);

#endif
