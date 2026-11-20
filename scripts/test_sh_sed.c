/* Host test and driver for src/sh_sed.c; run by scripts/test_sh_sed.py.
 * With no arguments it runs the checks. With arguments it acts as `sed [-n] [-e SCRIPT]...`
 * over stdin, so the wrapper can pin worked examples against a real sed. */
#include "sh_sed.h"

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

#define CHECK_STR(got, want)                                                        \
    do {                                                                            \
        if (strcmp((got), (want)) != 0) {                                           \
            printf("FAIL %s:%d: got \"%s\" want \"%s\"\n", __FILE__, __LINE__,    \
                   (got), (want));                                                  \
            failures++;                                                             \
        }                                                                           \
    } while (0)

typedef struct {
    char buf[4096];
    size_t used;
} out_t;

static void emit(void *ctx, const char *line, size_t len)
{
    out_t *o = ctx;

    memcpy(o->buf + o->used, line, len);
    o->used += len;
    o->buf[o->used++] = '\n';
    o->buf[o->used] = '\0';
}

/* Runs scripts over newline-separated input; the output is what the program printed. Returns
 * 0, or -1 with why filled when a script was refused. */
static int run_all(const char *const *scripts, int nscripts, int quiet, const char *input,
                   out_t *o, char *why, size_t whylen)
{
    sh_sed_t *p = sh_sed_new();
    char *copy = strdup(input);
    char *line = copy;
    int i, rc = 0;

    o->used = 0;
    o->buf[0] = '\0';
    for (i = 0; i < nscripts; i++) {
        if (sh_sed_add(p, scripts[i], why, whylen) != 0) {
            rc = -1;
            break;
        }
    }
    while (rc == 0 && *line != '\0') {
        char *nl = strchr(line, '\n');
        int r;

        if (nl != NULL) {
            *nl = '\0';
        }
        r = sh_sed_apply(p, line, quiet, emit, o);
        if (r < 0) {
            rc = -2;
            break;
        }
        if (r == SH_SED_QUIT || nl == NULL) {
            break;
        }
        line = nl + 1;
    }
    free(copy);
    sh_sed_free(p);
    return rc;
}

/* One script over input, the common case. */
static const char *sed1(const char *script, int quiet, const char *input)
{
    static out_t o;
    char why[128];
    const char *one[1];

    one[0] = script;
    if (run_all(one, 1, quiet, input, &o, why, sizeof(why)) != 0) {
        snprintf(o.buf, sizeof(o.buf), "<refused: %s>", why);
    }
    return o.buf;
}

/* The refusal text for a script, "" when it parsed. */
static const char *refusal(const char *script)
{
    static char why[128];
    sh_sed_t *p = sh_sed_new();

    why[0] = '\0';
    if (sh_sed_add(p, script, why, sizeof(why)) == 0) {
        why[0] = '\0';
    }
    sh_sed_free(p);
    return why;
}

static void test_substitute(void)
{
    /* AC1: first match per line, all with g, other lines unchanged. */
    CHECK_STR(sed1("s/old/new/", 0, "old old\nkeep\nold\n"), "new old\nkeep\nnew\n");
    CHECK_STR(sed1("s/old/new/g", 0, "old old\nkeep\nold\n"), "new new\nkeep\nnew\n");
    CHECK_STR(sed1("s/a/b/", 0, "xyz\n"), "xyz\n");
    /* AC2: any delimiter; \delim is the delimiter itself. */
    CHECK_STR(sed1("s|/ux0/a|/ux0/b|", 0, "/ux0/a/x\n"), "/ux0/b/x\n");
    CHECK_STR(sed1("s,a\\,b,X,", 0, "a,b\n"), "X\n");
    CHECK_STR(sed1("s#a#b/c#", 0, "a\n"), "b/c\n");
    /* The grep engine's own syntax, unchanged. */
    CHECK_STR(sed1("s/[0-9]\\{2,\\}/N/g", 0, "a1b22c333\n"), "a1bNcN\n");
    CHECK_STR(sed1("s/a\\+/X/", 0, "baaac\n"), "bXc\n");
    CHECK_STR(sed1("s/^ux0/UX0/", 0, "ux0 ux0\n"), "UX0 ux0\n");
    CHECK_STR(sed1("s/x$/Y/g", 0, "xx\n"), "xY\n");
    /* & is the match, \& and \\ are themselves. */
    CHECK_STR(sed1("s/b*c/[&]/", 0, "abbcd\n"), "a[bbc]d\n");
    CHECK_STR(sed1("s/a/\\&\\\\/", 0, "a\n"), "&\\\n");
    /* An empty replacement deletes the match. */
    CHECK_STR(sed1("s/b//g", 0, "abcb\n"), "ac\n");
}

static void test_empty_matches(void)
{
    /* Insert at the start and end, the common idioms. */
    CHECK_STR(sed1("s/^/> /", 0, "a\n\n"), "> a\n> \n");
    CHECK_STR(sed1("s/$/ </", 0, "a\n"), "a <\n");
    CHECK_STR(sed1("s/^$/EMPTY/", 0, "a\n\nb\n"), "a\nEMPTY\nb\n");
    /* GNU's walk over empty matches. */
    CHECK_STR(sed1("s/x*/-/g", 0, "abc\n"), "-a-b-c-\n");
    CHECK_STR(sed1("s/a*/x/g", 0, "baaac\n"), "xbxcx\n");
    CHECK_STR(sed1("s/x*/-/", 0, "abc\n"), "-abc\n");
    CHECK_STR(sed1("s/a*/x/g", 0, "\n"), "x\n");
}

static void test_commands(void)
{
    /* AC3 */
    CHECK_STR(sed1("p", 0, "a\nb\n"), "a\na\nb\nb\n");
    CHECK_STR(sed1("p", 1, "a\nb\n"), "a\nb\n");
    CHECK_STR(sed1("d", 0, "a\nb\n"), "");
    CHECK_STR(sed1("s/a/X/;d", 0, "a\n"), "");
    CHECK_STR(sed1("q", 0, "a\nb\nc\n"), "a\n");
    CHECK_STR(sed1("q", 1, "a\nb\nc\n"), "");
    CHECK_STR(sed1("s/a/X/;p;q", 1, "a\nb\n"), "X\n");
    /* d ends the cycle: nothing after it runs, not even autoprint. */
    CHECK_STR(sed1("d;p", 0, "a\n"), "");
    /* Commands see earlier substitutions. */
    CHECK_STR(sed1("s/a/b/;s/b/c/", 0, "a\n"), "c\n");
    /* Separators and spacing. */
    CHECK_STR(sed1("s/a/b/ ; s/b/c/\np", 0, "a\n"), "c\nc\n");
    CHECK_STR(sed1("  p  ", 1, "a\n"), "a\n");
    CHECK_STR(sed1("p # note\ns/a/b/", 1, "a\n"), "a\n");
    CHECK_STR(sed1("", 0, "a\nb\n"), "a\nb\n");
}

static void test_repeated_scripts(void)
{
    /* AC4: later scripts see the earlier ones' result. */
    static out_t o;
    char why[128];
    const char *two[] = {"s/a/b/", "s/b/c/"};
    const char *swapped[] = {"s/b/c/", "s/a/b/"};
    const char *bad[] = {"s/a/b/", "s/a/b"};

    CHECK(run_all(two, 2, 0, "a\n", &o, why, sizeof(why)) == 0);
    CHECK_STR(o.buf, "c\n");
    CHECK(run_all(swapped, 2, 0, "a\n", &o, why, sizeof(why)) == 0);
    CHECK_STR(o.buf, "b\n");
    /* A script that fails to parse stops the run before any line is read. */
    CHECK(run_all(bad, 2, 0, "a\n", &o, why, sizeof(why)) == -1);
    CHECK_STR(o.buf, "");
    CHECK(strstr(why, "unterminated") != NULL);
}

static void test_refusals(void)
{
    /* AC7: each names what it did not understand. */
    CHECK(strstr(refusal("1,5d"), "address") != NULL);
    CHECK(strstr(refusal("1,5d"), "1,5d") != NULL);
    CHECK(strstr(refusal("3p"), "address") != NULL);
    CHECK(strstr(refusal("$d"), "address") != NULL);
    CHECK(strstr(refusal("/x/d"), "address") != NULL);
    CHECK(strstr(refusal("1!d"), "address") != NULL);
    CHECK(strstr(refusal("h"), "hold space") != NULL);
    CHECK(strstr(refusal("G"), "hold space") != NULL);
    CHECK(strstr(refusal("x"), "hold space") != NULL);
    CHECK(strstr(refusal(":a"), "label") != NULL);
    CHECK(strstr(refusal("ba"), "branch") != NULL);
    CHECK(strstr(refusal("ta"), "branch") != NULL);
    CHECK(strstr(refusal("{p}"), "block") != NULL);
    CHECK(strstr(refusal("y/a/b/"), "'y'") != NULL);
    CHECK(strstr(refusal("a text"), "'a'") != NULL);
    CHECK(strstr(refusal("w out"), "'w'") != NULL);
    CHECK(strstr(refusal("q5"), "q") != NULL);
    CHECK(strstr(refusal("pp"), "extra") != NULL);
    CHECK(refusal("p;;d")[0] == '\0');
    /* s: syntax, flags, escapes the engine cannot honor. */
    CHECK(strstr(refusal("s/a/b"), "unterminated") != NULL);
    CHECK(strstr(refusal("s/a"), "unterminated") != NULL);
    CHECK(strstr(refusal("s"), "unterminated") != NULL);
    CHECK(strstr(refusal("s/a/b/p"), "flag 'p'") != NULL);
    CHECK(strstr(refusal("s/a/b/2"), "flag '2'") != NULL);
    CHECK(strstr(refusal("s/a/b/w x"), "flag 'w'") != NULL);
    CHECK(strstr(refusal("s/a/\\1/"), "back-reference") != NULL);
    CHECK(strstr(refusal("s/a/\\n/"), "\\n") != NULL);
    CHECK(strstr(refusal("s//b/"), "empty") != NULL);
    CHECK(strstr(refusal("s/\\(a/b/"), "regular expression") != NULL);
    CHECK(strstr(refusal("s\\a\\b\\"), "delimiter") != NULL);
    /* The accepted ones stay accepted. */
    CHECK(refusal("s/a/b/g")[0] == '\0');
    CHECK(refusal("s/a/b/g;p")[0] == '\0');
}

static void test_bounds(void)
{
    sh_sed_t *p = sh_sed_new();
    char why[128];
    char script[SH_SED_REPL_MAX + 64];
    int i;

    /* More commands than the program holds are refused, and the refused one adds nothing. */
    for (i = 0; i < SH_SED_CMD_MAX; i++) {
        CHECK(sh_sed_add(p, "p", why, sizeof(why)) == 0);
    }
    CHECK(sh_sed_add(p, "p", why, sizeof(why)) == -1);
    CHECK(strstr(why, "too many") != NULL);
    sh_sed_free(p);

    /* A replacement over the cap is refused. */
    memset(script, 'x', sizeof(script));
    memcpy(script, "s/a/", 4);
    memcpy(script + 4 + SH_SED_REPL_MAX + 1, "/", 2);
    p = sh_sed_new();
    CHECK(sh_sed_add(p, script, why, sizeof(why)) == -1);
    CHECK(strstr(why, "too long") != NULL);
    sh_sed_free(p);

    /* A partly parsed script adds nothing: the first command must not survive the second's error. */
    {
        static out_t o;
        const char *one[] = {"p;s/a/b"};

        CHECK(run_all(one, 1, 0, "a\n", &o, why, sizeof(why)) == -1);
        CHECK_STR(o.buf, "");
    }

    /* Growth past the pattern space is an error, not a truncation. */
    {
        char line[2100];
        out_t *o = calloc(1, sizeof(*o));
        sh_sed_t *q = sh_sed_new();
        char w[128];

        memset(line, 'a', sizeof(line) - 1);
        line[sizeof(line) - 1] = '\0';
        CHECK(sh_sed_add(q, "s/a/abcdefgh/g", w, sizeof(w)) == 0);
        CHECK(sh_sed_apply(q, line, 0, emit, o) == -1 && errno == ERANGE);
        CHECK(o->used == 0);
        free(o);
        sh_sed_free(q);
    }
}

/* Stdin as lines, the way cmd_sed will feed it: a last line with no newline still counts. */
static void test_unterminated_last_line(void)
{
    CHECK_STR(sed1("s/a/b/", 0, "a"), "b\n");
}

static void emit_stdout(void *ctx, const char *line, size_t len)
{
    (void)ctx;
    fwrite(line, 1, len, stdout);
    fputc('\n', stdout);
}

static int driver(int argc, char **argv)
{
    sh_sed_t *p = sh_sed_new();
    char why[128], line[SH_SED_SPACE_MAX];
    int quiet = 0, i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-n") == 0) {
            quiet = 1;
        } else if (strcmp(argv[i], "-e") == 0 && i + 1 < argc) {
            if (sh_sed_add(p, argv[++i], why, sizeof(why)) != 0) {
                printf("sed: %s\n", why);
                return 1;
            }
        }
    }
    while (fgets(line, sizeof(line), stdin) != NULL) {
        size_t n = strlen(line);
        int r;

        if (n > 0 && line[n - 1] == '\n') {
            line[n - 1] = '\0';
        }
        r = sh_sed_apply(p, line, quiet, emit_stdout, NULL);
        if (r == SH_SED_QUIT) {
            break;
        }
    }
    sh_sed_free(p);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc > 1) {
        return driver(argc, argv);
    }
    test_substitute();
    test_empty_matches();
    test_commands();
    test_repeated_scripts();
    test_refusals();
    test_bounds();
    test_unterminated_last_line();

    if (failures == 0) {
        printf("PASS sh_sed\n");
    }
    return failures != 0;
}
