/* Host test for src/sh_stdin.c; driven by scripts/test_sh_stdin.py. */
#include "sh_stdin.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

static int failures;

#define CHECK(cond)                                                               \
    do {                                                                          \
        if (!(cond)) {                                                            \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                \
            failures++;                                                           \
        }                                                                         \
    } while (0)

static ssize_t file_reader(void *ctx, void *buf, size_t n)
{
    size_t got = fread(buf, 1, n, (FILE *)ctx);

    if (got == 0 && ferror((FILE *)ctx)) {
        return -1;
    }
    return (ssize_t)got;
}

static FILE *file_with(const unsigned char *data, size_t len)
{
    FILE *f = tmpfile();

    if (f != NULL) {
        fwrite(data, 1, len, f);
        rewind(f);
    }
    return f;
}

/* Reads the whole source through a small buffer, so a chunk boundary is crossed. */
static size_t drain(sh_stdin_t *in, unsigned char *out, size_t cap)
{
    size_t total = 0, got;
    char chunk[4096];

    while ((got = sh_stdin_read(in, chunk, sizeof(chunk))) > 0 && total + got <= cap) {
        memcpy(out + total, chunk, got);
        total += got;
    }
    return total;
}

int main(void)
{
    sh_stdin_t in;
    unsigned char *big, *back;
    size_t i, n = 200000;
    char one[8];
    FILE *f;

    sh_stdin_init(&in);
    CHECK(sh_stdin_read(&in, one, sizeof(one)) == 0);

    big = malloc(n);
    back = malloc(n);
    for (i = 0; i < n; i++) {
        big[i] = (unsigned char)(i * 31 + 7);
    }
    f = file_with(big, n);
    CHECK(sh_stdin_load(&in, file_reader, f, 1024 * 1024) == 0);
    fclose(f);
    CHECK(drain(&in, back, n) == n);
    CHECK(memcmp(big, back, n) == 0);
    CHECK(sh_stdin_read(&in, one, sizeof(one)) == 0);

    f = file_with(big, 100);
    CHECK(sh_stdin_load(&in, file_reader, f, 100) == 0);
    fclose(f);
    CHECK(drain(&in, back, n) == 100);

    f = file_with(big, 101);
    errno = 0;
    CHECK(sh_stdin_load(&in, file_reader, f, 100) == -1);
    CHECK(errno == EFBIG);
    fclose(f);
    CHECK(sh_stdin_read(&in, one, sizeof(one)) == 0);

    f = file_with(big, 0);
    CHECK(sh_stdin_load(&in, file_reader, f, 100) == 0);
    fclose(f);
    CHECK(sh_stdin_read(&in, one, sizeof(one)) == 0);

    sh_stdin_free(&in);
    sh_stdin_free(&in);
    CHECK(sh_stdin_read(&in, one, sizeof(one)) == 0);
    free(big);
    free(back);

    if (failures == 0) {
        printf("PASS sh_stdin\n");
    }
    return failures != 0;
}
