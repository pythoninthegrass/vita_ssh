/* Host test for src/sh_du.c; driven by scripts/test_sh_du.py. */
#include "sh_du.h"

#include <stdio.h>
#include <string.h>

static int failures;

static void expect_kb(long long bytes, long long want)
{
    long long got = sh_du_kb(bytes);

    if (got != want) {
        printf("FAIL sh_du_kb(%lld) = %lld, want %lld\n", bytes, got, want);
        failures++;
    }
}

static void expect_human(long long kb, const char *want)
{
    char buf[16];

    sh_du_human(buf, sizeof(buf), kb);
    if (strcmp(buf, want) != 0) {
        printf("FAIL sh_du_human(%lld) = \"%s\", want \"%s\"\n", kb, buf, want);
        failures++;
    }
}

int main(void)
{
    char tiny[3] = "xx";

    expect_kb(0, 0);
    expect_kb(1, 1);
    expect_kb(1024, 1);
    expect_kb(1025, 2);
    expect_kb(2048, 2);
    expect_kb(1LL << 40, 1LL << 30);

    expect_human(0, "0");
    expect_human(1, "1.0K");
    expect_human(4, "4.0K");
    expect_human(9, "9.0K");
    expect_human(10, "10K");
    expect_human(1023, "1023K");
    expect_human(1024, "1.0M");
    expect_human(1025, "1.1M");
    expect_human(2355, "2.3M");
    expect_human(10 * 1024, "10M");
    expect_human(254 * 1024, "254M");
    expect_human(254 * 1024 + 1, "255M");
    expect_human(1023 * 1024 + 1, "1.0G");
    expect_human(1024LL * 1024, "1.0G");
    expect_human(1024LL * 1024 * 1024, "1.0T");
    expect_human(5000LL * 1024 * 1024 * 1024, "5000T");

    /* a short buffer is cut, never overrun (ASan catches the overrun) */
    sh_du_human(tiny, sizeof(tiny), 254 * 1024);
    if (strcmp(tiny, "25") != 0) {
        printf("FAIL short buffer gave \"%s\"\n", tiny);
        failures++;
    }

    if (failures == 0) {
        printf("sh_du: ok\n");
    }
    return failures != 0;
}
