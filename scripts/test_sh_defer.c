/* Host test for src/sh_defer.c; driven by scripts/test_sh_defer.py. */
#include "sh_defer.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(cond)                                                                 \
    do {                                                                            \
        if (!(cond)) {                                                              \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                 \
            failures++;                                                             \
        }                                                                           \
    } while (0)

static int add_fails(sh_defer_t *d, const char *data, size_t len, int want)
{
    errno = 0;
    return sh_defer_add(d, data, len) == -1 && errno == want;
}

static void test_order_and_reset(void)
{
    sh_defer_t d;

    sh_defer_init(&d);
    CHECK(sh_defer_len(&d) == 0);
    CHECK(sh_defer_add(&d, "echo ", 5) == 0);
    CHECK(sh_defer_add(&d, "MID1", 4) == 0);
    CHECK(sh_defer_len(&d) == 9);
    CHECK(memcmp(sh_defer_data(&d), "echo MID1", 9) == 0);
    sh_defer_reset(&d);
    CHECK(sh_defer_len(&d) == 0);
    CHECK(sh_defer_add(&d, "x", 1) == 0);
    CHECK(sh_defer_len(&d) == 1 && sh_defer_data(&d)[0] == 'x');
}

static void test_binary_bytes(void)
{
    sh_defer_t d;
    const char raw[] = {'\r', '\n', 0, '\033', '[', 'K'};

    sh_defer_init(&d);
    CHECK(sh_defer_add(&d, raw, sizeof(raw)) == 0);
    CHECK(sh_defer_len(&d) == sizeof(raw));
    CHECK(memcmp(sh_defer_data(&d), raw, sizeof(raw)) == 0);
}

static void test_bound_is_all_or_nothing(void)
{
    sh_defer_t d;
    char fill[SH_DEFER_MAX];

    memset(fill, 'a', sizeof(fill));
    sh_defer_init(&d);
    CHECK(sh_defer_add(&d, fill, SH_DEFER_MAX - 1) == 0);
    CHECK(add_fails(&d, "bc", 2, ENOSPC));
    CHECK(sh_defer_len(&d) == SH_DEFER_MAX - 1);
    CHECK(sh_defer_add(&d, "b", 1) == 0);
    CHECK(sh_defer_len(&d) == SH_DEFER_MAX);
    CHECK(add_fails(&d, "c", 1, ENOSPC));
    CHECK(sh_defer_data(&d)[SH_DEFER_MAX - 1] == 'b');
    CHECK(add_fails(&d, fill, SH_DEFER_MAX + 1, ENOSPC));
}

static void test_refused_write_leaves_earlier_bytes(void)
{
    sh_defer_t d;
    char big[SH_DEFER_MAX];

    memset(big, 'z', sizeof(big));
    sh_defer_init(&d);
    CHECK(sh_defer_add(&d, "keep", 4) == 0);
    CHECK(add_fails(&d, big, sizeof(big), ENOSPC));
    CHECK(sh_defer_len(&d) == 4 && memcmp(sh_defer_data(&d), "keep", 4) == 0);
}

static void test_invalid_and_empty(void)
{
    sh_defer_t d;

    sh_defer_init(&d);
    CHECK(add_fails(NULL, "a", 1, EINVAL));
    CHECK(add_fails(&d, NULL, 1, EINVAL));
    CHECK(sh_defer_add(&d, "", 0) == 0);
    CHECK(sh_defer_add(&d, NULL, 0) == 0);
    CHECK(sh_defer_len(&d) == 0);
}

int main(void)
{
    test_order_and_reset();
    test_binary_bytes();
    test_bound_is_all_or_nothing();
    test_refused_write_leaves_earlier_bytes();
    test_invalid_and_empty();
    if (failures == 0) {
        printf("sh_defer: all checks passed\n");
    }
    return failures == 0 ? 0 : 1;
}
