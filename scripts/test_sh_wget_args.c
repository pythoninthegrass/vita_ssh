/* Host test for src/sh_wget_args.c; driven by scripts/test_sh_wget_args.py. */
#include "sh_wget_args.h"

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

#define PARSE(rc, a, msg, ...)                                                      \
    do {                                                                            \
        char *av_[] = {"wget", __VA_ARGS__};                                        \
        memset(&(a), 0, sizeof(a));                                                 \
        msg[0] = '\0';                                                              \
        (rc) = sh_wget_parse((int)(sizeof(av_) / sizeof(av_[0])), av_, &(a), msg, sizeof(msg)); \
    } while (0)

static void test_ok(void)
{
    sh_wget_args_t a;
    char msg[160];
    int rc;

    PARSE(rc, a, msg, "https://h/x.bin");
    CHECK(rc == 0 && strcmp(a.url, "https://h/x.bin") == 0 && !a.out_file && !a.to_terminal && !a.quiet &&
          !a.insecure && !a.use_dispo);
    PARSE(rc, a, msg, "-q", "https://h/x");
    CHECK(rc == 0 && a.quiet);
    PARSE(rc, a, msg, "--quiet", "https://h/x");
    CHECK(rc == 0 && a.quiet);
    PARSE(rc, a, msg, "-O", "out.bin", "https://h/x");
    CHECK(rc == 0 && a.out_file && strcmp(a.out_file, "out.bin") == 0 && !a.to_terminal);
    PARSE(rc, a, msg, "-Oout.bin", "https://h/x");
    CHECK(rc == 0 && a.out_file && strcmp(a.out_file, "out.bin") == 0);
    PARSE(rc, a, msg, "-qO", "out.bin", "https://h/x");
    CHECK(rc == 0 && a.quiet && a.out_file && strcmp(a.out_file, "out.bin") == 0);
    PARSE(rc, a, msg, "--output-document=out.bin", "https://h/x");
    CHECK(rc == 0 && a.out_file && strcmp(a.out_file, "out.bin") == 0);
    PARSE(rc, a, msg, "--output-document", "out.bin", "https://h/x");
    CHECK(rc == 0 && a.out_file && strcmp(a.out_file, "out.bin") == 0);
    PARSE(rc, a, msg, "-O", "-", "https://h/x");
    CHECK(rc == 0 && a.to_terminal && !a.out_file);
    PARSE(rc, a, msg, "--no-check-certificate", "https://h/x");
    CHECK(rc == 0 && a.insecure);
    PARSE(rc, a, msg, "--content-disposition", "https://h/x");
    CHECK(rc == 0 && a.use_dispo);
    PARSE(rc, a, msg, "https://h/x", "-q");
    CHECK(rc == 0 && a.quiet && strcmp(a.url, "https://h/x") == 0);
    PARSE(rc, a, msg, "--", "-weird-url");
    CHECK(rc == 0 && strcmp(a.url, "-weird-url") == 0);
    PARSE(rc, a, msg, "-O", "a", "-O", "b", "https://h/x");
    CHECK(rc == 0 && strcmp(a.out_file, "b") == 0);
}

static void test_refused(void)
{
    sh_wget_args_t a;
    char msg[160];
    int rc;

    PARSE(rc, a, msg, "-r", "https://h/");
    CHECK(rc == -1 && strstr(msg, "-r") && strstr(msg, "recursive") && strstr(msg, "not supported"));
    PARSE(rc, a, msg, "--recursive", "https://h/");
    CHECK(rc == -1 && strstr(msg, "recursive"));
    PARSE(rc, a, msg, "-p", "https://h/");
    CHECK(rc == -1 && strstr(msg, "-p") && strstr(msg, "page requisites") && strstr(msg, "not supported"));
    PARSE(rc, a, msg, "--page-requisites", "https://h/");
    CHECK(rc == -1 && strstr(msg, "page requisites"));
    PARSE(rc, a, msg, "-m", "https://h/");
    CHECK(rc == -1 && strstr(msg, "mirror") && strstr(msg, "not supported"));
    PARSE(rc, a, msg, "--mirror", "https://h/");
    CHECK(rc == -1 && strstr(msg, "mirror"));
    /* refused wherever it sits in a cluster, and before a missing URL is noticed */
    PARSE(rc, a, msg, "-qr", "https://h/");
    CHECK(rc == -1 && strstr(msg, "recursive"));
    PARSE(rc, a, msg, "-r");
    CHECK(rc == -1 && strstr(msg, "recursive"));
    PARSE(rc, a, msg, "-O", "-r", "https://h/");
    CHECK(rc == 0 && strcmp(a.out_file, "-r") == 0);
}

static void test_errors(void)
{
    sh_wget_args_t a;
    char msg[160];
    int rc;

    PARSE(rc, a, msg, "-z", "https://h/x");
    CHECK(rc == -1 && strstr(msg, "-z") && strstr(msg, "unknown option"));
    PARSE(rc, a, msg, "--bogus", "https://h/x");
    CHECK(rc == -1 && strstr(msg, "--bogus") && strstr(msg, "unknown option"));
    PARSE(rc, a, msg, "-O");
    CHECK(rc == -1 && strstr(msg, "-O needs a file name"));
    PARSE(rc, a, msg, "--output-document");
    CHECK(rc == -1 && strstr(msg, "needs a file name"));
    PARSE(rc, a, msg, "https://h/a", "https://h/b");
    CHECK(rc == -1 && strstr(msg, "only one URL"));
    PARSE(rc, a, msg, "-q");
    CHECK(rc == 1);
    PARSE(rc, a, msg, "-O", "x");
    CHECK(rc == 1);
    PARSE(rc, a, msg, "-O", "");
    CHECK(rc == -1 && strstr(msg, "needs a file name"));
    /* a small message buffer is cut, never overrun */
    {
        char tiny[8];
        char *av[] = {"wget", "--a-very-long-unknown-option-name", "u"};

        CHECK(sh_wget_parse(3, av, &a, tiny, sizeof(tiny)) == -1 && strlen(tiny) < sizeof(tiny));
    }
}

int main(void)
{
    test_ok();
    test_refused();
    test_errors();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("ok\n");
    return 0;
}
