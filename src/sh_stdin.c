#include "sh_stdin.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#define LOAD_CHUNK 4096

void sh_stdin_init(sh_stdin_t *in)
{
    in->data = NULL;
    in->len = 0;
    in->off = 0;
    in->cap = 0;
}

void sh_stdin_free(sh_stdin_t *in)
{
    free(in->data);
    sh_stdin_init(in);
}

int sh_stdin_load(sh_stdin_t *in, sh_stdin_reader_t rd, void *ctx, size_t max)
{
    ssize_t got;

    sh_stdin_free(in);
    for (;;) {
        char *grown;

        if (in->len == in->cap) {
            size_t want = in->cap == 0 ? LOAD_CHUNK : in->cap * 2;

            if (want > max + 1) {
                want = max + 1;
            }
            grown = want > in->cap ? realloc(in->data, want) : NULL;
            if (grown == NULL) {
                int saved = in->len > max ? EFBIG : ENOMEM;

                sh_stdin_free(in);
                errno = saved;
                return -1;
            }
            in->data = grown;
            in->cap = want;
        }
        got = rd(ctx, in->data + in->len, in->cap - in->len);
        if (got < 0) {
            int err = errno;

            sh_stdin_free(in);
            errno = err;
            return -1;
        }
        in->len += (size_t)got;
        if (in->len > max) {
            sh_stdin_free(in);
            errno = EFBIG;
            return -1;
        }
        if (got == 0) {
            return 0;
        }
    }
}

int sh_stdin_append(sh_stdin_t *in, const char *data, size_t n, size_t max)
{
    if (n > max - in->len || in->len > max) {
        sh_stdin_free(in);
        errno = EFBIG;
        return -1;
    }
    if (in->len + n > in->cap) {
        size_t want = in->cap == 0 ? LOAD_CHUNK : in->cap;
        char *grown;

        while (want < in->len + n) {
            want *= 2;
        }
        grown = realloc(in->data, want);
        if (grown == NULL) {
            sh_stdin_free(in);
            errno = ENOMEM;
            return -1;
        }
        in->data = grown;
        in->cap = want;
    }
    if (n > 0) {
        memcpy(in->data + in->len, data, n);
        in->len += n;
    }
    return 0;
}

void sh_stdin_move(sh_stdin_t *dst, sh_stdin_t *src)
{
    sh_stdin_free(dst);
    *dst = *src;
    dst->off = 0;
    sh_stdin_init(src);
}

size_t sh_stdin_read(sh_stdin_t *in, char *buf, size_t n)
{
    size_t left = in->len - in->off;

    if (n > left) {
        n = left;
    }
    if (n > 0) {
        memcpy(buf, in->data + in->off, n);
        in->off += n;
    }
    return n;
}
