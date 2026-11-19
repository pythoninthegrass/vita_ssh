/* Host test for src/sh_exit.c; driven by scripts/test_sh_exit.py. */
#include "sh_exit.h"

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

/* Runs `exit <args...>` after a command that left last; returns the verdict and the status it chose. */
static sh_exit_verdict_t run(int last, int nargs, const char *a, const char *b, int *status)
{
    char *argv[3] = {"exit", (char *)a, (char *)b};

    *status = -1;
    return sh_exit_parse(1 + nargs, argv, last, status);
}

static void expect_end(int last, const char *arg, int want)
{
    int status;
    sh_exit_verdict_t v = run(last, arg != NULL, arg, NULL, &status);

    if (v != SH_EXIT_END || status != want) {
        printf("FAIL exit %s after %d: verdict %d status %d, want END %d\n", arg != NULL ? arg : "", last,
               (int)v, status, want);
        failures++;
    }
}

static void expect_bad(const char *arg)
{
    int status;
    sh_exit_verdict_t v = run(7, 1, arg, NULL, &status);

    if (v != SH_EXIT_BAD_NUMBER || status != 2) {
        printf("FAIL exit '%s': verdict %d status %d, want BAD_NUMBER 2\n", arg, (int)v, status);
        failures++;
    }
}

static void test_no_argument_uses_last_status(void)
{
    expect_end(0, NULL, 0);
    expect_end(1, NULL, 1);
    expect_end(127, NULL, 127);
    expect_end(130, NULL, 130);
}

static void test_status_is_reduced_modulo_256(void)
{
    expect_end(9, "0", 0);
    expect_end(9, "3", 3);
    expect_end(9, "255", 255);
    expect_end(9, "256", 0);
    expect_end(9, "257", 1);
    expect_end(9, "00012", 12);
    expect_end(9, "2147483647", 255);
}

static void test_bad_numbers(void)
{
    expect_bad("abc");
    expect_bad("");
    expect_bad("-1");
    expect_bad("+1");
    expect_bad(" 1");
    expect_bad("1 ");
    expect_bad("1x");
    expect_bad("0x10");
    expect_bad("1.5");
    expect_bad("2147483648");
    expect_bad("99999999999999999999999999");
}

static void test_too_many_arguments(void)
{
    int status;

    CHECK(run(0, 2, "1", "2", &status) == SH_EXIT_TOO_MANY && status == 1);
    CHECK(run(0, 2, "abc", "2", &status) == SH_EXIT_BAD_NUMBER && status == 2);
}

int main(void)
{
    test_no_argument_uses_last_status();
    test_status_is_reduced_modulo_256();
    test_bad_numbers();
    test_too_many_arguments();
    if (failures == 0) {
        printf("sh_exit: all checks passed\n");
    }
    return failures == 0 ? 0 : 1;
}
