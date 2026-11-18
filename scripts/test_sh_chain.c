/* Host test for src/sh_chain.c; driven by scripts/test_sh_chain.py. */
#include "sh_chain.h"

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

#define MAXW 64

/* One line as the tokenizer would hand it over: words, each with the do-not-glob bit. */
typedef struct {
    char *argv[MAXW];
    int quoted[MAXW];
    int argc;
} line_t;

static void put(line_t *l, const char *w, int quoted)
{
    l->argv[l->argc] = (char *)w;
    l->quoted[l->argc] = quoted;
    l->argc++;
}

/* Builds a line from a space-separated spec; a word starting with ' is quoted, the quote is dropped. */
static void build(line_t *l, char *spec)
{
    char *w;

    l->argc = 0;
    for (w = strtok(spec, " "); w != NULL; w = strtok(NULL, " ")) {
        if (w[0] == '\'') {
            put(l, w + 1, 1);
        } else {
            put(l, w, 0);
        }
    }
}

static int check(const char *spec, sh_chain_op_t *bad)
{
    char copy[256];
    line_t l;

    snprintf(copy, sizeof(copy), "%s", spec);
    build(&l, copy);
    return sh_chain_check(l.argv, l.quoted, l.argc, bad);
}

static void test_operator_words(void)
{
    CHECK(sh_chain_op(";", 0) == SH_CHAIN_SEQ);
    CHECK(sh_chain_op("&&", 0) == SH_CHAIN_AND);
    CHECK(sh_chain_op("||", 0) == SH_CHAIN_OR);

    /* Quoted, glued or longer than the operator, the word is text. */
    CHECK(sh_chain_op(";", 1) == SH_CHAIN_NONE);
    CHECK(sh_chain_op("&&", 1) == SH_CHAIN_NONE);
    CHECK(sh_chain_op("||", 1) == SH_CHAIN_NONE);
    CHECK(sh_chain_op("a;b", 0) == SH_CHAIN_NONE);
    CHECK(sh_chain_op("hi;", 0) == SH_CHAIN_NONE);
    CHECK(sh_chain_op(";;", 0) == SH_CHAIN_NONE);
    CHECK(sh_chain_op("&&&", 0) == SH_CHAIN_NONE);
    CHECK(sh_chain_op("&", 0) == SH_CHAIN_NONE);
    CHECK(sh_chain_op("|", 0) == SH_CHAIN_NONE); /* a pipe is not a chain operator */
    CHECK(sh_chain_op("", 0) == SH_CHAIN_NONE);

    CHECK(strcmp(sh_chain_op_name(SH_CHAIN_SEQ), ";") == 0);
    CHECK(strcmp(sh_chain_op_name(SH_CHAIN_AND), "&&") == 0);
    CHECK(strcmp(sh_chain_op_name(SH_CHAIN_OR), "||") == 0);
}

static void test_run_or_skip(void)
{
    int st;

    /* AC1: ; always runs. */
    for (st = 0; st < 3; st++) {
        CHECK(sh_chain_should_run(SH_CHAIN_SEQ, st) == 1);
    }
    /* AC2: && runs on 0 only. */
    CHECK(sh_chain_should_run(SH_CHAIN_AND, 0) == 1);
    CHECK(sh_chain_should_run(SH_CHAIN_AND, 1) == 0);
    CHECK(sh_chain_should_run(SH_CHAIN_AND, 127) == 0);
    CHECK(sh_chain_should_run(SH_CHAIN_AND, 130) == 0);
    /* AC3: || runs on nonzero only. */
    CHECK(sh_chain_should_run(SH_CHAIN_OR, 0) == 0);
    CHECK(sh_chain_should_run(SH_CHAIN_OR, 1) == 1);
    CHECK(sh_chain_should_run(SH_CHAIN_OR, 127) == 1);
    CHECK(sh_chain_should_run(SH_CHAIN_OR, 130) == 1);
}

/* AC4's shape, left to right with equal precedence: a && b || c. */
static void test_left_to_right(void)
{
    /* ls /nope && echo yes || echo no: status 1, yes skipped (status stays 1), no runs. */
    int status = 1;

    if (sh_chain_should_run(SH_CHAIN_AND, status)) {
        status = 0;
    }
    CHECK(status == 1);
    CHECK(sh_chain_should_run(SH_CHAIN_OR, status) == 1);

    /* ls /ux0 && echo yes || echo no: status 0, yes runs (0), no is skipped. */
    status = 0;
    if (sh_chain_should_run(SH_CHAIN_AND, status)) {
        status = 0;
    }
    CHECK(sh_chain_should_run(SH_CHAIN_OR, status) == 0);
}

static void test_check_valid(void)
{
    sh_chain_op_t bad = SH_CHAIN_NONE;

    CHECK(check("", &bad) == 1);
    CHECK(check("echo a", &bad) == 1);
    CHECK(check("echo a ; echo b", &bad) == 2);
    CHECK(check("echo a && echo b || echo c", &bad) == 3);
    CHECK(check("true ; false ; true", &bad) == 3);
    CHECK(check("echo a | cat ; echo b", &bad) == 2); /* a pipe is part of one command */
    CHECK(check("echo 'a; b", NULL) == 1);             /* quoted word is text */
    CHECK(check("echo '; echo '&& echo '||", NULL) == 1);
    CHECK(check("echo a;b", NULL) == 1); /* glued stays text */
}

static void test_check_syntax(void)
{
    sh_chain_op_t bad = SH_CHAIN_NONE;

    /* AC10: leading, doubled, trailing. */
    CHECK(check("; echo a", &bad) == SH_CHAIN_SYNTAX && bad == SH_CHAIN_SEQ);
    CHECK(check("&& echo a", &bad) == SH_CHAIN_SYNTAX && bad == SH_CHAIN_AND);
    CHECK(check("|| echo a", &bad) == SH_CHAIN_SYNTAX && bad == SH_CHAIN_OR);
    CHECK(check("echo a ; ; echo b", &bad) == SH_CHAIN_SYNTAX && bad == SH_CHAIN_SEQ);
    CHECK(check("echo a && && echo b", &bad) == SH_CHAIN_SYNTAX && bad == SH_CHAIN_AND);
    CHECK(check("echo a && ; echo b", &bad) == SH_CHAIN_SYNTAX && bad == SH_CHAIN_SEQ);
    CHECK(check("echo a ; || echo b", &bad) == SH_CHAIN_SYNTAX && bad == SH_CHAIN_OR);
    CHECK(check("echo a &&", &bad) == SH_CHAIN_SYNTAX && bad == SH_CHAIN_AND);
    CHECK(check("echo a ||", &bad) == SH_CHAIN_SYNTAX && bad == SH_CHAIN_OR);
    CHECK(check(";", &bad) == SH_CHAIN_SYNTAX && bad == SH_CHAIN_SEQ);
    CHECK(check("echo a ; ;", &bad) == SH_CHAIN_SYNTAX && bad == SH_CHAIN_SEQ);
    CHECK(check("echo a && ;", &bad) == SH_CHAIN_SYNTAX && bad == SH_CHAIN_SEQ);
    CHECK(check("echo a ; &&", &bad) == SH_CHAIN_SYNTAX && bad == SH_CHAIN_AND);

    /* A NULL bad is allowed. */
    CHECK(check("; echo a", NULL) == SH_CHAIN_SYNTAX);

    /* A quoted operator is a word, so it neither starts, doubles nor ends a chain. */
    CHECK(check("'; echo a", NULL) == 1);
    CHECK(check("echo a ; '&&", NULL) == 2);
}

static void test_words(void)
{
    char copy[64];
    line_t l;

    /* The command words of a valid line: everything but a single trailing ;. */
    snprintf(copy, sizeof(copy), "echo a ;");
    build(&l, copy);
    CHECK(sh_chain_words(l.argv, l.quoted, l.argc) == 2);

    snprintf(copy, sizeof(copy), "echo a ; echo b ;");
    build(&l, copy);
    CHECK(sh_chain_words(l.argv, l.quoted, l.argc) == 5);

    snprintf(copy, sizeof(copy), "echo a ; echo b");
    build(&l, copy);
    CHECK(sh_chain_words(l.argv, l.quoted, l.argc) == l.argc);

    snprintf(copy, sizeof(copy), "echo ';");
    build(&l, copy);
    CHECK(sh_chain_words(l.argv, l.quoted, l.argc) == l.argc); /* quoted ; is a word */

    CHECK(sh_chain_words(l.argv, l.quoted, 0) == 0);
}

static void test_check_limit(void)
{
    char spec[512];
    int n, i;
    sh_chain_op_t bad = SH_CHAIN_NONE;

    for (n = 1; n <= SH_CHAIN_MAX + 2; n++) {
        spec[0] = '\0';
        for (i = 0; i < n; i++) {
            strcat(spec, i == 0 ? "true" : " ; true");
        }
        if (n <= SH_CHAIN_MAX) {
            CHECK(check(spec, &bad) == n);
        } else {
            CHECK(check(spec, &bad) == SH_CHAIN_TOO_LONG);
        }
    }

    /* A trailing ; adds no command, so it does not push a full chain over the limit. */
    spec[0] = '\0';
    for (i = 0; i < SH_CHAIN_MAX; i++) {
        strcat(spec, i == 0 ? "true" : " ; true");
    }
    strcat(spec, " ;");
    CHECK(check(spec, &bad) == SH_CHAIN_MAX);

    /* A syntax error is reported ahead of a too-long chain. */
    spec[0] = '\0';
    for (i = 0; i < SH_CHAIN_MAX + 1; i++) {
        strcat(spec, "true ; ");
    }
    strcat(spec, ";");
    CHECK(check(spec, &bad) == SH_CHAIN_SYNTAX && bad == SH_CHAIN_SEQ);
}

/* Every word sequence up to length 7 from six kinds of word, checked against a model written separately. */
static const char *kinds[] = {"w", ";", "&&", "||", "';", "'&&"};
#define NKINDS 6

static int is_op_kind(int k)
{
    return k >= 1 && k <= 3;
}

static void test_exhaustive(void)
{
    int len;

    for (len = 0; len <= 7; len++) {
        long total = 1, code;
        int i;

        for (i = 0; i < len; i++) {
            total *= NKINDS;
        }
        for (code = 0; code < total; code++) {
            int seq[8], c = (int)code, ops = 0, expect, expect_bad = 0;
            char spec[128];
            sh_chain_op_t bad = SH_CHAIN_NONE;
            int got;

            spec[0] = '\0';
            for (i = 0; i < len; i++) {
                seq[i] = c % NKINDS;
                c /= NKINDS;
                if (i > 0) {
                    strcat(spec, " ");
                }
                strcat(spec, kinds[seq[i]]);
                ops += is_op_kind(seq[i]);
            }

            expect = ops + 1;
            if (len > 0 && seq[len - 1] == 1) {
                expect--; /* a trailing ; starts no command */
            }
            for (i = 0; i < len; i++) {
                int leading = (i == 0 && is_op_kind(seq[i]));
                int doubled = (i > 0 && is_op_kind(seq[i]) && is_op_kind(seq[i - 1]));
                int trailing = (i == len - 1 && is_op_kind(seq[i]) && seq[i] != 1);

                if (leading || doubled || trailing) {
                    expect = SH_CHAIN_SYNTAX;
                    expect_bad = seq[i]; /* kinds 1..3 are ; && || */
                    break;
                }
            }
            if (expect > SH_CHAIN_MAX) {
                expect = SH_CHAIN_TOO_LONG;
            }

            if (expect > 0) {
                char copy[128];
                line_t l;
                int want_words = len - (len > 0 && seq[len - 1] == 1 && expect <= SH_CHAIN_MAX ? 1 : 0);

                snprintf(copy, sizeof(copy), "%s", spec);
                build(&l, copy);
                if (expect <= SH_CHAIN_MAX && len > 0 &&
                    sh_chain_words(l.argv, l.quoted, l.argc) != want_words) {
                    printf("FAIL exhaustive words \"%s\"\n", spec);
                    failures++;
                }
            }

            got = check(spec, &bad);
            if (got != expect) {
                printf("FAIL exhaustive \"%s\": got %d, want %d\n", spec, got, expect);
                failures++;
            } else if (expect == SH_CHAIN_SYNTAX && (int)bad != expect_bad) {
                printf("FAIL exhaustive \"%s\": bad %d, want %d\n", spec, (int)bad, expect_bad);
                failures++;
            }
        }
    }
}

int main(void)
{
    test_operator_words();
    test_run_or_skip();
    test_left_to_right();
    test_check_valid();
    test_words();
    test_check_syntax();
    test_check_limit();
    test_exhaustive();
    if (failures != 0) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("ok\n");
    return 0;
}
