/* Host test for src/sh_env.c; driven by scripts/test_sh_env.py. */
#include "sh_env.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

#define CHECK(cond)                                                                 \
    do {                                                                            \
        if (!(cond)) {                                                              \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                 \
            failures++;                                                             \
        }                                                                           \
    } while (0)

static int set_fails(sh_env_table_t *t, const char *name, const char *value, int want)
{
    errno = 0;
    return sh_env_set(t, name, value) == -1 && errno == want;
}

static void test_names(void)
{
    char longname[SH_ENV_NAME_MAX + 2];

    CHECK(sh_env_name_ok("TERM"));
    CHECK(sh_env_name_ok("_x"));
    CHECK(sh_env_name_ok("a1_B2"));
    CHECK(!sh_env_name_ok(""));
    CHECK(!sh_env_name_ok(NULL));
    CHECK(!sh_env_name_ok("1A"));
    CHECK(!sh_env_name_ok("A=B"));
    CHECK(!sh_env_name_ok("A B"));
    CHECK(!sh_env_name_ok("A-B"));
    memset(longname, 'A', sizeof(longname) - 1);
    longname[sizeof(longname) - 1] = '\0';
    CHECK(!sh_env_name_ok(longname));
    longname[SH_ENV_NAME_MAX] = '\0';
    CHECK(sh_env_name_ok(longname));
}

static void test_set_get(void)
{
    sh_env_table_t *t = sh_env_new();

    CHECK(t != NULL);
    CHECK(sh_env_get(t, "TERM") == NULL);
    CHECK(sh_env_count(t) == 0);
    CHECK(sh_env_set(t, "TERM", "xterm") == 0);
    CHECK(strcmp(sh_env_get(t, "TERM"), "xterm") == 0);
    CHECK(sh_env_set(t, "TERM", "vt100") == 0);
    CHECK(strcmp(sh_env_get(t, "TERM"), "vt100") == 0);
    CHECK(sh_env_count(t) == 1);
    CHECK(sh_env_set(t, "EMPTY", "") == 0);
    CHECK(sh_env_get(t, "EMPTY") != NULL && sh_env_get(t, "EMPTY")[0] == '\0');
    CHECK(sh_env_get(t, "TERM2") == NULL);
    CHECK(sh_env_get(NULL, "TERM") == NULL);
    sh_env_free(t);
}

static void test_order(void)
{
    sh_env_table_t *t = sh_env_new();

    sh_env_set(t, "B", "1");
    sh_env_set(t, "A", "2");
    sh_env_set(t, "B", "3");
    CHECK(sh_env_count(t) == 2);
    CHECK(strcmp(sh_env_name_at(t, 0), "B") == 0);
    CHECK(strcmp(sh_env_value_at(t, 0), "3") == 0);
    CHECK(strcmp(sh_env_name_at(t, 1), "A") == 0);
    CHECK(sh_env_name_at(t, 2) == NULL);
    CHECK(sh_env_value_at(t, -1) == NULL);
    CHECK(sh_env_count(NULL) == 0);
    sh_env_free(t);
}

static void test_clean(void)
{
    sh_env_table_t *t = sh_env_new();
    char big[SH_ENV_VALUE_MAX + 2];

    CHECK(sh_env_set(t, "V", "a\033[2Jb\x07\n\r\t\x7f" "c") == 0);
    CHECK(strcmp(sh_env_get(t, "V"), "a[2Jbc") == 0);
    CHECK(sh_env_set(t, "UTF", "caf\xc3\xa9") == 0);
    CHECK(strcmp(sh_env_get(t, "UTF"), "caf\xc3\xa9") == 0);

    memset(big, 'x', sizeof(big) - 1);
    big[sizeof(big) - 1] = '\0';
    CHECK(set_fails(t, "BIG", big, ENAMETOOLONG));
    CHECK(sh_env_get(t, "BIG") == NULL);
    big[SH_ENV_VALUE_MAX] = '\0';
    CHECK(sh_env_set(t, "BIG", big) == 0);
    CHECK(strlen(sh_env_get(t, "BIG")) == SH_ENV_VALUE_MAX);
    sh_env_free(t);
}

static void test_strip_ctrl(void)
{
    char buf[8];

    CHECK(sh_env_strip_ctrl(buf, sizeof(buf), "alice") == 5);
    CHECK(strcmp(buf, "alice") == 0);
    CHECK(sh_env_strip_ctrl(buf, sizeof(buf), "a\033[2Jb\x07\n\r\t\x7f" "c") == 6);
    CHECK(strcmp(buf, "a[2Jbc") == 0);
    CHECK(sh_env_strip_ctrl(buf, sizeof(buf), "caf\xc3\xa9") == 5);
    CHECK(strcmp(buf, "caf\xc3\xa9") == 0);
    CHECK(sh_env_strip_ctrl(buf, sizeof(buf), "\033\x01\x7f") == 0);
    CHECK(buf[0] == '\0');
    CHECK(sh_env_strip_ctrl(buf, sizeof(buf), "") == 0);
    CHECK(sh_env_strip_ctrl(buf, sizeof(buf), "\033abcdefghij") == 7);
    CHECK(strcmp(buf, "abcdefg") == 0);
    CHECK(sh_env_strip_ctrl(buf, 1, "abc") == 0);
    CHECK(buf[0] == '\0');
    CHECK(sh_env_strip_ctrl(buf, sizeof(buf), NULL) == 0);
    CHECK(buf[0] == '\0');
}

static void test_errors(void)
{
    sh_env_table_t *t = sh_env_new();
    char name[SH_ENV_NAME_MAX + 1];
    int i;

    CHECK(set_fails(t, "1A", "x", EINVAL));
    CHECK(set_fails(t, "", "x", EINVAL));
    CHECK(set_fails(t, "A", NULL, EINVAL));
    CHECK(set_fails(NULL, "A", "x", EINVAL));
    for (i = 0; i < SH_ENV_MAX; i++) {
        snprintf(name, sizeof(name), "V%d", i);
        CHECK(sh_env_set(t, name, "x") == 0);
    }
    CHECK(set_fails(t, "ONE_MORE", "x", ENOSPC));
    CHECK(sh_env_set(t, "V0", "y") == 0);
    CHECK(strcmp(sh_env_get(t, "V0"), "y") == 0);
    CHECK(sh_env_count(t) == SH_ENV_MAX);
    sh_env_free(t);
}

static void test_isolation(void)
{
    sh_env_table_t *a = sh_env_new(), *b = sh_env_new();

    sh_env_set(a, "TERM", "xterm");
    sh_env_set(b, "TERM", "vt100");
    CHECK(strcmp(sh_env_get(a, "TERM"), "xterm") == 0);
    CHECK(strcmp(sh_env_get(b, "TERM"), "vt100") == 0);
    sh_env_free(a);
    sh_env_free(b);
}

int main(void)
{
    test_names();
    test_set_get();
    test_order();
    test_clean();
    test_strip_ctrl();
    test_errors();
    test_isolation();
    if (failures == 0) {
        printf("sh_env: all checks passed\n");
    }
    return failures == 0 ? 0 : 1;
}
