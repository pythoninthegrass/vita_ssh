/* A bounded buffer for terminal output that a libssh callback produced while a command was running: no I/O and no Vita APIs. */
#ifndef SH_DEFER_H
#define SH_DEFER_H

#include <stddef.h>

/* Most bytes held. Enough for several typed lines of echo; a write past it is refused whole. */
#define SH_DEFER_MAX 2048

typedef struct {
    char buf[SH_DEFER_MAX];
    size_t len;
} sh_defer_t;

void sh_defer_init(sh_defer_t *d);

size_t sh_defer_len(const sh_defer_t *d);

/* The held bytes, sh_defer_len() of them, not NUL terminated. */
const char *sh_defer_data(const sh_defer_t *d);

/* Appends len bytes, all or none, so an escape sequence is never cut in half. Returns 0, or -1 with
 * errno: EINVAL (NULL buffer, or NULL data with len > 0), ENOSPC (it would pass SH_DEFER_MAX; nothing
 * held is touched). */
int sh_defer_add(sh_defer_t *d, const char *data, size_t len);

/* Forgets the held bytes. */
void sh_defer_reset(sh_defer_t *d);

#endif
