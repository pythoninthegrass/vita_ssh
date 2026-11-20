/* Host test and driver for src/sh_awk.c; run by scripts/test_sh_awk.py.
 * With no arguments it runs the checks. With arguments it acts as `awk [-F SEP] PROGRAM` over
 * stdin, so the wrapper can pin worked examples against a real awk. */
#include "sh_awk.h"
#include "sh_grep.h"

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
    char buf[65536];
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

/* Runs a program over newline-separated input. Returns 0, -1 with why filled when the program
 * was refused, or -2 with errno from sh_awk_apply when a line failed. */
static int run(const char *fs, const char *program, const char *input, out_t *o, char *why,
               size_t whylen)
{
    sh_awk_t *p = sh_awk_new();
    char *copy = strdup(input);
    char *line = copy;
    int rc = 0;

    o->used = 0;
    o->buf[0] = '\0';
    if (sh_awk_compile(p, program, fs, why, whylen) != 0) {
        rc = -1;
    }
    while (rc == 0 && *line != '\0') {
        char *nl = strchr(line, '\n');

        if (nl != NULL) {
            *nl = '\0';
        }
        if (sh_awk_apply(p, line, emit, o) != 0) {
            rc = -2;
            break;
        }
        if (nl == NULL) {
            break;
        }
        line = nl + 1;
    }
    free(copy);
    sh_awk_free(p);
    return rc;
}

/* The output of a program, or "<refused: why>" when it did not compile. */
static const char *awk(const char *fs, const char *program, const char *input)
{
    static out_t o;
    char why[160];

    if (run(fs, program, input, &o, why, sizeof(why)) == -1) {
        snprintf(o.buf, sizeof(o.buf), "<refused: %s>", why);
    }
    return o.buf;
}

/* The refusal text for a program, "" when it compiled. */
static const char *refusal(const char *fs, const char *program)
{
    static char why[160];
    sh_awk_t *p = sh_awk_new();

    why[0] = '\0';
    if (sh_awk_compile(p, program, fs, why, sizeof(why)) == 0) {
        why[0] = '\0';
    }
    sh_awk_free(p);
    return why;
}

static void test_fields(void)
{
    /* AC1: print selected fields joined by one space; $0 is the whole line. */
    CHECK_STR(awk(NULL, "{print $1, $3}", "a b c d\ne f g\n"), "a c\ne g\n");
    CHECK_STR(awk(NULL, "{print $0}", "a  b\tc\n"), "a  b\tc\n");
    CHECK_STR(awk(NULL, "{print}", "a  b\n"), "a  b\n");
    CHECK_STR(awk(NULL, "{print $3, $1}", "a b c\n"), "c a\n");
    /* Runs of blanks and tabs are one separator and the ends are trimmed. */
    CHECK_STR(awk(NULL, "{print $1, $2}", "  a \t b  \n"), "a b\n");
    /* A field past NF is empty, so a join leaves its space. */
    CHECK_STR(awk(NULL, "{print $1, $5}", "a b\n"), "a \n");
    CHECK_STR(awk(NULL, "{print $2}", "\n"), "\n");
    /* $NF is the last field; an empty line has none, so it is $0. */
    CHECK_STR(awk(NULL, "{print $NF}", "a b c\n"), "c\n");
    CHECK_STR(awk(NULL, "{print $NF}", "\n"), "\n");
    /* Literals print as awk prints them. */
    CHECK_STR(awk(NULL, "{print \"x\", 1.50, 007, 1e3}", "a\n"), "x 1.5 7 1000\n");
    CHECK_STR(awk(NULL, "{print \"a\\tb\\\\\"}", "a\n"), "a\tb\\\n");
    /* Several statements in one action. */
    CHECK_STR(awk(NULL, "{print $1; print $2}", "a b\n"), "a\nb\n");
    CHECK_STR(awk(NULL, "{ print $1 ; }", "a b\n"), "a\n");
}

static void test_separator(void)
{
    /* AC2: a single character splits there and keeps empty fields. */
    CHECK_STR(awk(":", "{print $2}", "a:b:c\n"), "b\n");
    CHECK_STR(awk(":", "{print NF}", "a::b\n"), "3\n");
    CHECK_STR(awk(":", "{print $2 \"x\"}", "a::b\n"), "<refused: unsupported: string concatenation (separate arguments with commas)>");
    CHECK_STR(awk(":", "{print $1, $2, $3}", "a::b\n"), "a  b\n");
    CHECK_STR(awk(":", "{print NF}", "a:\n"), "2\n");
    CHECK_STR(awk(":", "{print NF}", ":\n"), "2\n");
    CHECK_STR(awk(":", "{print NF}", "\n"), "0\n");
    CHECK_STR(awk("|", "{print $2}", "a|b|c\n"), "b\n");
    CHECK_STR(awk(".", "{print $2}", "a.b.c\n"), "b\n");
    CHECK_STR(awk(",", "{print $3}", "x,y,\n"), "\n");
    /* \t is a tab; a lone blank is the default. */
    CHECK_STR(awk("\\t", "{print $2}", "a b\tc d\te\n"), "c d\n");
    CHECK_STR(awk(" ", "{print $2}", "a   b\n"), "b\n");
    /* With a separator, blanks are ordinary bytes. */
    CHECK_STR(awk(",", "{print $2}", "a, b,c\n"), " b\n");
}

static void test_nf_nr(void)
{
    /* AC3: NF and NR as print arguments and in patterns. */
    CHECK_STR(awk(NULL, "{print NR, NF}", "a b\nc\n\nd e f\n"), "1 2\n2 1\n3 0\n4 3\n");
    CHECK_STR(awk(NULL, "NR > 2", "a\nb\nc\nd\n"), "c\nd\n");
    CHECK_STR(awk(NULL, "NR == 2 {print $0}", "a\nb\nc\n"), "b\n");
    CHECK_STR(awk(NULL, "NF == 2", "a b\nc\nd e\n"), "a b\nd e\n");
    CHECK_STR(awk(NULL, "NF >= 2 {print NR}", "a b\nc\nd e f\n"), "1\n3\n");
    CHECK_STR(awk(NULL, "NF != 1", "a b\nc\nd e\n"), "a b\nd e\n");
    CHECK_STR(awk(NULL, "NR <= 1", "a\nb\n"), "a\n");
    CHECK_STR(awk(NULL, "NR < 3 {print NR}", "a\nb\nc\n"), "1\n2\n");
}

static void test_patterns(void)
{
    /* AC4: a regex gates the action; no action means print the line. */
    CHECK_STR(awk(NULL, "/b/", "abc\nxyz\nb\n"), "abc\nb\n");
    CHECK_STR(awk(NULL, "/b/ {print $1}", "a b\nxyz\n"), "a\n");
    CHECK_STR(awk(NULL, "/^a/", "ab\nba\n"), "ab\n");
    CHECK_STR(awk(NULL, "/a$/", "ab\nba\n"), "ba\n");
    CHECK_STR(awk(NULL, "/a\\/b/", "a/b\nab\n"), "a/b\n");
    CHECK_STR(awk(NULL, "/[0-9]/", "a1\nb\n"), "a1\n");
    /* The regex is extended, as in awk, not the engine's basic. */
    CHECK_STR(awk(NULL, "/^[0-9]+$/", "12\n1a\n+\n"), "12\n");
    CHECK_STR(awk(NULL, "/^ab?c$/", "ac\nabc\nabbc\n"), "ac\nabc\n");
    CHECK_STR(awk(NULL, "/^(ab|cd)$/", "ab\ncd\nabcd\n"), "ab\ncd\n");
    CHECK_STR(awk(NULL, "/a{2}/", "a\naa\n"), "aa\n");
    CHECK_STR(awk(NULL, "/a\\+b/", "a+b\naab\n"), "a+b\n");
    CHECK_STR(awk(NULL, "/\\(x\\)/", "(x)\nx\n"), "(x)\n");
    CHECK_STR(awk(NULL, "/a|b/", "a\nb\nc\n"), "a\nb\n");
    /* A comparison of a field against a literal. */
    CHECK_STR(awk(NULL, "$3 == \"x\"", "a b x\na b y\n"), "a b x\n");
    CHECK_STR(awk(NULL, "$3 == \"x\" {print $1}", "a b x\nc d y\n"), "a\n");
    CHECK_STR(awk(NULL, "$1 != \"a\"", "a\nb\n"), "b\n");
    CHECK_STR(awk(NULL, "$2 > 10", "a 5\nb 20\nc 10\n"), "b 20\n");
    CHECK_STR(awk(NULL, "$2 >= 10 {print $1}", "a 5\nb 20\nc 10\n"), "b\nc\n");
    CHECK_STR(awk(NULL, "$2 < 10 {print $1}", "a 5\nb 20\nc 10\n"), "a\n");
    CHECK_STR(awk(NULL, "$2 <= 10 {print $1}", "a 5\nb 20\nc 10\n"), "a\nc\n");
    CHECK_STR(awk(NULL, "$2 == 5", "a 5\nb 5.0\nc 05\nd x\n"), "a 5\nb 5.0\nc 05\n");
    CHECK_STR(awk(NULL, "$1 == 1.0", "1\n1.0\n1e0\nabc\n"), "1\n1.0\n1e0\n");
    CHECK_STR(awk(NULL, "$2 > -1", "a 0\nb -2\n"), "a 0\n");
    /* A field that is not a number compares as a string; so does a string literal. */
    CHECK_STR(awk(NULL, "$1 > 5", "abc\n10\n9\n"), "abc\n10\n9\n");
    CHECK_STR(awk(NULL, "$1 < \"b\"", "a\nc\n"), "a\n");
    CHECK_STR(awk(NULL, "$1 == \"10\"", "10\n10.0\n"), "10\n");
    /* A literal on the left works as well. */
    CHECK_STR(awk(NULL, "5 < $1", "4\n6\n"), "6\n");
    CHECK_STR(awk(NULL, "NR == NF", "a\nb c\nd\n"), "a\nb c\n");
    /* A field past NF is the empty string, not 0: real awk answers these the same way. */
    CHECK_STR(awk(NULL, "$3 == \"\"", "a b\na b c\n"), "a b\n");
    CHECK_STR(awk(NULL, "$3 == 0", "a b\na b 0\n"), "a b 0\n");
    CHECK_STR(awk(NULL, "$3 < 1", "a b\na b 0\na b 2\n"), "a b\na b 0\n");
    CHECK_STR(awk(NULL, "$2 > -1", "a 0\nc\n"), "a 0\n");
}

static void test_refusals(void)
{
    /* AC6: named constructs, refused before any line is read. */
    CHECK_STR(refusal(NULL, "BEGIN {print 1}"), "unsupported: BEGIN");
    CHECK_STR(refusal(NULL, "END {print NR}"), "unsupported: END");
    CHECK_STR(refusal(NULL, "{x = 1}"), "unsupported: assignment");
    CHECK_STR(refusal(NULL, "{x += 1}"), "unsupported: assignment");
    CHECK_STR(refusal(NULL, "{n++}"), "unsupported: assignment");
    CHECK_STR(refusal(NULL, "{$1 = \"a\"}"), "unsupported: assignment");
    CHECK_STR(refusal(NULL, "{NF = 2}"), "unsupported: assignment");
    CHECK_STR(refusal(NULL, "{if ($1) print}"), "unsupported: if");
    CHECK_STR(refusal(NULL, "{while (1) print}"), "unsupported: while");
    CHECK_STR(refusal(NULL, "{for (;;) print}"), "unsupported: for");
    CHECK_STR(refusal(NULL, "{do print; while (0)}"), "unsupported: do");
    CHECK_STR(refusal(NULL, "function f(x) {return x}"), "unsupported: function");
    CHECK_STR(refusal(NULL, "{print length($0)}"), "unsupported: function call length");
    CHECK_STR(refusal(NULL, "{print substr($0, 2)}"), "unsupported: function call substr");
    CHECK_STR(refusal(NULL, "{printf \"%s\", $1}"), "unsupported: printf");
    CHECK_STR(refusal(NULL, "{next}"), "unsupported: next");
    CHECK_STR(refusal(NULL, "{exit}"), "unsupported: exit");
    CHECK_STR(refusal(NULL, "{getline}"), "unsupported: getline");
    CHECK_STR(refusal(NULL, "{print x}"), "unsupported: variable x");
    CHECK_STR(refusal(NULL, "$3 == x"), "unsupported: variable x");
    CHECK_STR(refusal(NULL, "{print $x}"), "unsupported: variable x");
    CHECK_STR(refusal(NULL, "{print $(NF-1)}"), "unsupported: $(expression)");
    CHECK_STR(refusal(NULL, "{print NF-1}"), "unsupported: arithmetic");
    CHECK_STR(refusal(NULL, "{print $1 + $2}"), "unsupported: arithmetic");
    CHECK_STR(refusal(NULL, "{print $1 * 2}"), "unsupported: arithmetic");
    CHECK_STR(refusal(NULL, "{print $1 / 2}"), "unsupported: arithmetic");
    CHECK_STR(refusal(NULL, "{print $1 % 2}"), "unsupported: arithmetic");
    CHECK_STR(refusal(NULL, "{print $1 > \"/ux0/data/x\"}"), "unsupported: output redirection");
    CHECK_STR(refusal(NULL, "{print $1 >> \"x\"}"), "unsupported: output redirection");
    CHECK_STR(refusal(NULL, "{print $1 | \"sort\"}"), "unsupported: output redirection");
    CHECK_STR(refusal(NULL, "$1 ~ /a/"), "unsupported: ~");
    CHECK_STR(refusal(NULL, "$1 !~ /a/"), "unsupported: !~");
    CHECK_STR(refusal(NULL, "!/a/"), "unsupported: !");
    CHECK_STR(refusal(NULL, "/a/ && /b/"), "unsupported: &&");
    CHECK_STR(refusal(NULL, "/a/ || /b/"), "unsupported: ||");
    CHECK_STR(refusal(NULL, "/a/,/b/"), "unsupported: range pattern");
    CHECK_STR(refusal(NULL, "NR == 1 {print} NR == 2 {print}"), "unsupported: multiple rules");
    CHECK_STR(refusal(NULL, "/a/; /b/"), "unsupported: multiple rules");
    CHECK_STR(refusal(NULL, "{print $1}\n{print $2}"), "unsupported: multiple rules");
    CHECK_STR(refusal(NULL, "{print $1 ? 1 : 2}"), "unsupported: ?:");
    CHECK_STR(refusal(NULL, "{print $1 == 2}"), "unsupported: comparison in print");
    CHECK_STR(refusal(NULL, "{print($1)}"), "unsupported: parentheses");
    CHECK_STR(refusal(NULL, "{print $1 $2}"), "unsupported: string concatenation (separate arguments with commas)");
    CHECK_STR(refusal(NULL, "{print $1; getline x}"), "unsupported: getline");
    CHECK_STR(refusal("ab", "{print}"), "unsupported: -F with more than one character");
    CHECK_STR(refusal("[ ]+", "{print}"), "unsupported: -F with more than one character");
    CHECK_STR(refusal("t", "{print}"), "unsupported: -Ft (write -F '\\t' for a tab)");
    /* A program that is refused prints nothing, even when an earlier line would have matched. */
    CHECK_STR(awk(NULL, "{print $1; x = 1}", "a\nb\n"), "<refused: unsupported: assignment>");
}

static void test_syntax_errors(void)
{
    /* AC7: a program it does not understand is an error, not text. */
    CHECK(refusal(NULL, "")[0] != '\0');
    CHECK(refusal(NULL, "   ")[0] != '\0');
    CHECK(refusal(NULL, "{print $1")[0] != '\0');
    CHECK(refusal(NULL, "print $1")[0] != '\0');
    CHECK(refusal(NULL, "{print $}")[0] != '\0');
    CHECK(refusal(NULL, "{print \"abc}")[0] != '\0');
    CHECK(refusal(NULL, "/abc")[0] != '\0');
    CHECK(refusal(NULL, "$1")[0] != '\0');
    CHECK(refusal(NULL, "$1 ==")[0] != '\0');
    CHECK(refusal(NULL, "== 1")[0] != '\0');
    CHECK(refusal(NULL, "{print $1,}")[0] != '\0');
    CHECK(refusal(NULL, "{print ,$1}")[0] != '\0');
    CHECK(refusal(NULL, "{ foo bar }")[0] != '\0');
    CHECK(refusal(NULL, "{print $1} }")[0] != '\0');
    CHECK(refusal(NULL, "/a/ junk")[0] != '\0');
    CHECK(refusal(NULL, "/(/")[0] != '\0');
    CHECK(refusal(NULL, "{print \"a\\qb\"}")[0] != '\0');
    CHECK(refusal(NULL, "{print @}")[0] != '\0');
    CHECK_STR(refusal(NULL, "{print $1"), "missing }");
    CHECK_STR(refusal(NULL, ""), "empty program");
    CHECK_STR(refusal(NULL, "{print \"abc}"), "unterminated string");
    CHECK_STR(refusal(NULL, "/abc"), "unterminated regular expression");
    CHECK_STR(refusal(NULL, "$1"), "pattern must be /regex/ or a comparison");
    CHECK_STR(refusal(NULL, "{print $}"), "expected a field number after $");
}

static void test_bounds(void)
{
    /* AC8: a line with too many fields is reported, never cut. */
    char line[SH_AWK_FIELD_MAX * 2 + 16];
    char *q = line;
    out_t o;
    char why[160];
    int i;

    for (i = 0; i < SH_AWK_FIELD_MAX; i++) {
        *q++ = 'x';
        *q++ = ' ';
    }
    *q = '\0';
    CHECK(run(NULL, "{print NF}", line, &o, why, sizeof(why)) == 0);
    CHECK_STR(o.buf, "128\n");
    *q++ = 'y';
    *q = '\0';
    errno = 0;
    CHECK(run(NULL, "{print NF}", line, &o, why, sizeof(why)) == -2);
    CHECK(errno == ERANGE);
    CHECK_STR(o.buf, "");

    /* With a separator the bound is the same. */
    {
        char many[SH_AWK_FIELD_MAX * 2 + 16];
        char *m = many;

        for (i = 0; i < SH_AWK_FIELD_MAX; i++) {
            *m++ = ',';
        }
        *m = '\0';
        CHECK(run(",", "{print NF}", many, &o, why, sizeof(why)) == -2);
        CHECK(errno == ERANGE);
    }

    /* A print longer than the output bound is refused, not cut. */
    {
        char big[SH_GREP_LINE_MAX + 1];
        sh_awk_t *p = sh_awk_new();
        out_t out;
        int r;

        memset(big, 'a', sizeof(big) - 1);
        big[sizeof(big) - 1] = '\0';
        CHECK(sh_awk_compile(p, "{print $0, $0, $0}", NULL, why, sizeof(why)) == 0);
        out.used = 0;
        out.buf[0] = '\0';
        errno = 0;
        r = sh_awk_apply(p, big, emit, &out);
        CHECK(r == -1);
        CHECK(errno == ERANGE);
        CHECK(out.used == 0);
        sh_awk_free(p);
    }

    /* Too many print arguments or statements, and an over-long literal, are refused at compile. */
    CHECK(strstr(refusal(NULL, "{print $1,$1,$1,$1,$1,$1,$1,$1,$1,$1,$1,$1,$1,$1,$1,$1,$1}"), "too many") != NULL);
    CHECK(strstr(refusal(NULL, "{print 1;print 1;print 1;print 1;print 1;print 1;print 1;print 1;print 1}"), "too many") != NULL);
}

static void test_nr_across_runs(void)
{
    /* NR carries across files because the program object carries it, not the file loop. */
    sh_awk_t *p = sh_awk_new();
    out_t o;
    char why[160];

    o.used = 0;
    o.buf[0] = '\0';
    CHECK(sh_awk_compile(p, "{print NR}", NULL, why, sizeof(why)) == 0);
    CHECK(sh_awk_apply(p, "a", emit, &o) == 0);
    CHECK(sh_awk_apply(p, "b", emit, &o) == 0);
    CHECK_STR(o.buf, "1\n2\n");
    sh_awk_free(p);
}

static void emit_stdout(void *ctx, const char *line, size_t len)
{
    (void)ctx;
    fwrite(line, 1, len, stdout);
    fputc('\n', stdout);
}

/* `awk [-F SEP] PROGRAM` over stdin, for pinning against a real awk. */
static int driver(int argc, char **argv)
{
    const char *fs = NULL;
    char why[160];
    static char line[16384];
    sh_awk_t *p = sh_awk_new();
    int i = 1;

    if (i + 1 < argc && strcmp(argv[i], "-F") == 0) {
        fs = argv[i + 1];
        i += 2;
    }
    if (i >= argc || sh_awk_compile(p, argv[i], fs, why, sizeof(why)) != 0) {
        printf("awk: %s\n", i >= argc ? "missing program" : why);
        return 2;
    }
    while (fgets(line, sizeof(line), stdin) != NULL) {
        size_t n = strlen(line);

        if (n > 0 && line[n - 1] == '\n') {
            line[n - 1] = '\0';
        }
        if (sh_awk_apply(p, line, emit_stdout, NULL) != 0) {
            printf("awk: line too wide\n");
            return 1;
        }
    }
    sh_awk_free(p);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc > 1) {
        return driver(argc, argv);
    }
    test_fields();
    test_separator();
    test_nf_nr();
    test_patterns();
    test_refusals();
    test_syntax_errors();
    test_bounds();
    test_nr_across_runs();

    if (failures == 0) {
        printf("PASS sh_awk\n");
    }
    return failures != 0;
}
