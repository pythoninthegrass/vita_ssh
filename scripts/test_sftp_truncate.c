/* Host test for src/sftp_open_policy.h; driven by scripts/test_sftp_truncate.py. */
#include "sftp_open_policy.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failures;

#define CHECK(cond)                                                               \
    do {                                                                          \
        if (!(cond)) {                                                            \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                \
            failures++;                                                           \
        }                                                                         \
    } while (0)

/* Mirrors __wrap_open/__wrap_close: write to <target>.part when staged, then promote. */
static int upload(const char *target, int flags, const char *data, size_t len)
{
    char part[512];
    const char *dest = target;
    int fd;

    snprintf(part, sizeof(part), "%s.part", target);
    if (sftp_open_stages(flags)) {
        dest = part;
    }
    fd = open(dest, flags & ~O_TRUNC, 0644);
    if (fd < 0) {
        return -1;
    }
    if (write(fd, data, len) != (ssize_t)len) {
        close(fd);
        return -1;
    }
    close(fd);
    if (dest == part) {
        unlink(target);
        return rename(part, target);
    }
    return 0;
}

static long file_size(const char *path)
{
    FILE *f = fopen(path, "rb");
    long n;

    if (f == NULL) {
        return -1;
    }
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fclose(f);
    return n;
}

static int file_equals(const char *path, const char *data, size_t len)
{
    char buf[1024];
    FILE *f = fopen(path, "rb");
    size_t n;

    if (f == NULL) {
        return 0;
    }
    n = fread(buf, 1, sizeof(buf), f);
    fclose(f);
    return n == len && memcmp(buf, data, len) == 0;
}

static void overwrite_case(const char *dir, const char *name, size_t first, size_t second,
                           int flags)
{
    char path[512], a[600], b[600];

    snprintf(path, sizeof(path), "%s/%s", dir, name);
    memset(a, 'A', sizeof(a));
    memset(b, 'b', sizeof(b));
    CHECK(upload(path, flags, a, first) == 0);
    CHECK(file_size(path) == (long)first);
    CHECK(upload(path, flags, b, second) == 0);
    CHECK(file_size(path) == (long)second);
    CHECK(file_equals(path, b, second));
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : ".";
    const int scp_flags = O_WRONLY | O_CREAT;
    const int trunc_flags = O_WRONLY | O_CREAT | O_TRUNC;

    CHECK(sftp_open_stages(scp_flags));
    CHECK(sftp_open_stages(trunc_flags));
    CHECK(sftp_open_stages(O_RDWR | O_CREAT | O_TRUNC));
    CHECK(!sftp_open_stages(O_RDWR | O_CREAT));
    CHECK(!sftp_open_stages(O_WRONLY | O_CREAT | O_APPEND));
    CHECK(!sftp_open_stages(O_WRONLY | O_CREAT | O_EXCL));
    CHECK(!sftp_open_stages(O_WRONLY));
    CHECK(!sftp_open_stages(O_RDONLY));

    overwrite_case(dir, "shrink", 227, 6, scp_flags);
    overwrite_case(dir, "same", 100, 100, scp_flags);
    overwrite_case(dir, "grow", 6, 227, scp_flags);
    overwrite_case(dir, "shrink_trunc", 227, 6, trunc_flags);

    if (failures == 0) {
        printf("PASS sftp_open_policy\n");
    }
    return failures != 0;
}
