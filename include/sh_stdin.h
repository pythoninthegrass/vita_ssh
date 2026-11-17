/* A bounded, in-memory stdin source for one shell command. */
#ifndef SH_STDIN_H
#define SH_STDIN_H

#include <stddef.h>
#include <sys/types.h>

typedef struct {
    char *data;
    size_t len;
    size_t off;
    size_t cap;
} sh_stdin_t;

void sh_stdin_init(sh_stdin_t *in);

/* Returns the bytes read, 0 at the end, or -1 with errno set. */
typedef ssize_t (*sh_stdin_reader_t)(void *ctx, void *buf, size_t n);

/* Replaces the source with everything rd produces. Returns -1 with errno EFBIG when it
 * holds more than max bytes, or the read errno, and leaves the source empty. */
int sh_stdin_load(sh_stdin_t *in, sh_stdin_reader_t rd, void *ctx, size_t max);

/* Appends n bytes, growing the source. Returns -1 with errno EFBIG when the total would pass
 * max, or ENOMEM, and leaves the source empty. */
int sh_stdin_append(sh_stdin_t *in, const char *data, size_t n, size_t max);

/* Makes dst hold src's bytes, unread, in place of whatever dst held; src ends empty. */
void sh_stdin_move(sh_stdin_t *dst, sh_stdin_t *src);

/* Copies up to n bytes. Returns 0 at the end and for a source that was never loaded,
 * so a reader never waits. */
size_t sh_stdin_read(sh_stdin_t *in, char *buf, size_t n);

void sh_stdin_free(sh_stdin_t *in);

#endif
