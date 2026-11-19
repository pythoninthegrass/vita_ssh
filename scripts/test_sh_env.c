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

static void test_unset(void)
{
    sh_env_table_t *t = sh_env_new();

    sh_env_set(t, "A", "1");
    sh_env_set(t, "B", "2");
    sh_env_set(t, "C", "3");
    errno = 0;
    CHECK(sh_env_unset(t, "NOPE") == -1 && errno == ENOENT);
    CHECK(sh_env_unset(t, "B") == 0);
    CHECK(sh_env_get(t, "B") == NULL);
    CHECK(sh_env_count(t) == 2);
    CHECK(strcmp(sh_env_name_at(t, 0), "A") == 0 && strcmp(sh_env_name_at(t, 1), "C") == 0);
    CHECK(strcmp(sh_env_value_at(t, 1), "3") == 0);
    CHECK(sh_env_set(t, "B", "again") == 0);
    CHECK(strcmp(sh_env_name_at(t, 2), "B") == 0);
    errno = 0;
    CHECK(sh_env_unset(NULL, "A") == -1 && errno == EINVAL);
    sh_env_free(t);
}

static void test_readonly(void)
{
    sh_env_table_t *t = sh_env_new();
    const char *fixed[] = {"HOME", "PWD", "USER", "LOGNAME", "TMPDIR"};
    size_t i;

    for (i = 0; i < sizeof(fixed) / sizeof(fixed[0]); i++) {
        CHECK(sh_env_is_readonly(t, fixed[i]));
    }
    CHECK(!sh_env_is_readonly(t, "FOO"));
    CHECK(!sh_env_is_readonly(t, "home"));
    CHECK(!sh_env_is_readonly(NULL, "FOO"));
    CHECK(sh_env_is_readonly(NULL, "HOME"));
    errno = 0;
    CHECK(sh_env_lock(t, "TERM") == -1 && errno == ENOENT);
    sh_env_set(t, "TERM", "xterm");
    CHECK(!sh_env_is_readonly(t, "TERM"));
    CHECK(sh_env_lock(t, "TERM") == 0);
    CHECK(sh_env_is_readonly(t, "TERM"));
    CHECK(sh_env_set(t, "TERM", "vt100") == 0);
    CHECK(sh_env_is_readonly(t, "TERM"));
    CHECK(sh_env_unset(t, "TERM") == 0);
    CHECK(!sh_env_is_readonly(t, "TERM"));
    sh_env_set(t, "A", "1");
    sh_env_set(t, "B", "2");
    sh_env_lock(t, "B");
    sh_env_unset(t, "A");
    CHECK(sh_env_is_readonly(t, "B"));
    CHECK(!sh_env_is_readonly(t, "A"));
    sh_env_free(t);
}

static void test_snapshot(void)
{
    sh_env_table_t *t = sh_env_new(), *saved;

    sh_env_set(t, "A", "1");
    sh_env_set(t, "B", "2");
    sh_env_lock(t, "B");
    saved = sh_env_snapshot(t);
    CHECK(saved != NULL && saved != t);
    sh_env_set(t, "A", "changed");
    sh_env_set(t, "NEW", "x");
    sh_env_unset(t, "B");
    sh_env_restore(t, saved);
    CHECK(strcmp(sh_env_get(t, "A"), "1") == 0);
    CHECK(strcmp(sh_env_get(t, "B"), "2") == 0 && sh_env_is_readonly(t, "B"));
    CHECK(sh_env_get(t, "NEW") == NULL);
    CHECK(sh_env_count(t) == 2);
    CHECK(sh_env_snapshot(NULL) == NULL);
    sh_env_restore(NULL, NULL);
    sh_env_free(t);
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
    test_unset();
    test_readonly();
    test_snapshot();
    if (failures == 0) {
        printf("sh_env: all checks passed\n");
    }
    return failures == 0 ? 0 : 1;
}
