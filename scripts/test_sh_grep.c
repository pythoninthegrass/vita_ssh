/* Host test for src/sh_grep.c; driven by scripts/test_sh_grep.py. */
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

/* Compiles, matches one line, frees; returns the decision, or -2 if the pattern is invalid. */
static int run(const char *pattern, int flags, const char *line, int invert)
{
    sh_grep_t *re = sh_grep_compile(pattern, flags);
    int hit;

    if (re == NULL) {
        return -2;
    }
    hit = sh_grep_match(re, line, invert);
    sh_grep_free(re);
    return hit;
}

/* True when the pattern is rejected outright. */
static int rejected(const char *pattern, int flags)
{
    sh_grep_t *re = sh_grep_compile(pattern, flags);

    if (re == NULL) {
        return 1;
    }
    sh_grep_free(re);
    return 0;
}

static void test_bre_anchors_and_dots(void)
{
    /* grep 'a.c' matches abc: the case the honesty claim about the engine rests on. */
    CHECK(run("a.c", 0, "abc", 0) == 1);
    CHECK(run("a.c", 0, "ac", 0) == 0);
    CHECK(run("a.c", 0, "a\nc", 0) == 1); /* BRE . matches any byte, newline included */

    /* Unanchored by default, so the match may start anywhere in the line. */
    CHECK(run("ux0", 0, "app/ux0/log", 0) == 1);
    CHECK(run("^ux0", 0, "ux0:app/x", 0) == 1);
    CHECK(run("^ux0", 0, " /ux0/x", 0) == 0);
    CHECK(run("log$", 0, "syslog", 0) == 1);
    CHECK(run("log$", 0, "syslogs", 0) == 0);
    CHECK(run("^a$b", 0, "ab", 0) == 0);
    CHECK(run("^ab$", 0, "ab", 0) == 1);
    /* ^ and $ away from the ends are the bytes themselves, as BRE defines them. */
    CHECK(run("a^b", 0, "a^b", 0) == 1);
    CHECK(run("a$b", 0, "a$b", 0) == 1);
}

static void test_bre_quantifiers(void)
{
    /* "s" then zero or more "e": a bare "e" has no s, so it does not match. */
    CHECK(run("se*", 0, "e", 0) == 0);
    CHECK(run("se*", 0, "see", 0) == 1);
    CHECK(run("se*", 0, "se", 0) == 1);
    CHECK(run("se\\+", 0, "se", 0) == 1);
    CHECK(run("se\\+", 0, "s", 0) == 0);
    CHECK(run("ab\\?", 0, "a", 0) == 1);
    CHECK(run("ab\\?", 0, "ab", 0) == 1);
    CHECK(run("ab\\?", 0, "abc", 0) == 1);
    CHECK(run("a\\{2\\}", 0, "aa", 0) == 1);
    CHECK(run("a\\{2\\}", 0, "a", 0) == 0);
    CHECK(run("a\\{2,3\\}", 0, "aaa", 0) == 1);
    CHECK(run("a\\{2,3\\}", 0, "a", 0) == 0);
    CHECK(run("a\\{2,\\}", 0, "aaaa", 0) == 1);
    CHECK(run("a\\{2,\\}", 0, "a", 0) == 0);
    CHECK(run("\\(ab\\)*", 0, "abab", 0) == 1);
    CHECK(run("\\(ab\\)*", 0, "zzz", 0) == 1); /* zero copies is a match */
    /* An unescaped + is a literal byte in BRE; the operator is \+. */
    CHECK(run("\\(ab\\)+", 0, "abab", 0) == 0);
    CHECK(run("\\(ab\\)\\+", 0, "abab", 0) == 1);
    CHECK(run("\\(ab\\)\\+", 0, "zzz", 0) == 0);
    CHECK(run("\\(ab\\)+", 0, "zzz", 0) == 0);
    CHECK(run("\\(a\\)\\{2\\}b", 0, "aab", 0) == 1);
    CHECK(run("\\(a\\)\\{2\\}b", 0, "ab", 0) == 0);
}

static void test_bre_classes_and_alternation(void)
{
    CHECK(run("[uv]x0", 0, "ux0", 0) == 1);
    CHECK(run("[uv]x0", 0, "wx0", 0) == 0);
    CHECK(run("[^uv]x0", 0, "wx0", 0) == 1);
    CHECK(run("[^uv]x0", 0, "ux0", 0) == 0);
    CHECK(run("[a-z]\\+x", 0, "abcx", 0) == 1);
    CHECK(run("[a-z]+x", 0, "q+x", 0) == 1); /* an unescaped + is a literal byte */
    CHECK(run("[a-z]+x", 0, "[a-z]x", 0) == 0); /* no letter sits before the + */
    CHECK(run("[a-c]d", 0, "ed", 0) == 0);
    /* ']' first and '-' last are both literal members of the set. */
    CHECK(run("[]a]", 0, "]", 0) == 1);
    CHECK(run("[-a]", 0, "-", 0) == 1);
    CHECK(run("a\\|b", 0, "zzb", 0) == 1);
    CHECK(run("a\\|b", 0, "zzz", 0) == 0);
    CHECK(run("\\(ab\\)\\|c", 0, "zzab", 0) == 1);
    CHECK(run(".*", 0, "", 0) == 1);
    CHECK(run("", 0, "anything", 0) == 1); /* the empty pattern matches every line */
    CHECK(run("^", 0, "anything", 0) == 1);
    CHECK(run("$", 0, "anything", 0) == 1);
}

static void test_bre_literals(void)
{
    /* BRE takes + ? ( ) { } | as bytes unless escaped, unlike an extended engine. */
    CHECK(run("a+b", 0, "a+b", 0) == 1);
    CHECK(run("a+b", 0, "aab", 0) == 0);
    CHECK(run("(ab)c", 0, "(ab)c", 0) == 1);
    CHECK(run("a{2}", 0, "a{2}", 0) == 1);
    CHECK(run("a{2}", 0, "aab", 0) == 0);
    CHECK(run("a|b", 0, "a|b", 0) == 1);
    /* a\. is the two bytes a and .: a lone dot does not contain them. */
    CHECK(run("a\\.", 0, ".", 0) == 0);
    CHECK(run("a\\.", 0, "a.", 0) == 1);
    CHECK(run("a\\.", 0, "a", 0) == 0);
    CHECK(run("a\\\\b", 0, "a\\b", 0) == 1);
    CHECK(run("a\\*b", 0, "a*b", 0) == 1);
    CHECK(run("a\\*b", 0, "ab", 0) == 0);
}

static void test_icase(void)
{
    CHECK(run("UX0", SH_GREP_ICASE, "app/ux0/log", 0) == 1);
    CHECK(run("UX0", 0, "app/ux0/log", 0) == 0);
    CHECK(run("AbC", SH_GREP_ICASE, "xabcx", 0) == 1);
    CHECK(run("[a-z]BC", SH_GREP_ICASE, "ABC", 0) == 1);
    CHECK(run("[a-z]BC", 0, "ABC", 0) == 0);
    /* Folding is ASCII only: a byte >= 0x80 is compared as itself, independent of locale. */
    CHECK(run("\xc4", SH_GREP_ICASE, "\xc4", 0) == 1);
    CHECK(run("\xc4", SH_GREP_ICASE, "\xc5", 0) == 0);
}

static void test_invert(void)
{
    CHECK(run("a.c", 0, "abc", 1) == 0);
    CHECK(run("a.c", 0, "ac", 1) == 1);
    CHECK(run("", 0, "", 1) == 0); /* the pattern that matches all inverts to none */
}

static void test_bad_patterns(void)
{
    char err[96];

    /* An unescaped ( or ) is an ordinary byte in BRE. */
    CHECK(run("a(", 0, "a(", 0) == 1);
    CHECK(run("a(", 0, "abc", 0) == 0);
    CHECK(run("a)", 0, "a)", 0) == 1);
    CHECK(rejected("\\(a", 0));
    CHECK(rejected("[a-", 0));
    CHECK(rejected("[a", 0));
    CHECK(rejected("[z-a]", 0));       /* a descending range is a typo, not a set */
    CHECK(rejected("a\\{2,1\\}", 0));  /* and so are inverted interval bounds */
    CHECK(rejected("*a", 0));          /* nothing to repeat */
    CHECK(rejected("\\", 0));          /* trailing backslash */
    CHECK(rejected("a\\{", 0));        /* an interval that never closes */

    CHECK(sh_grep_compile("a\\(", 0) == NULL);
    CHECK(strcmp(sh_grep_compile_error(err, sizeof(err)), "") != 0);
    CHECK(sh_grep_compile(NULL, 0) == NULL);
    /* A pattern past the bound is refused rather than truncated into a different pattern. */
    {
        char *big = malloc(SH_GREP_PATTERN_MAX + 2);

        memset(big, 'a', SH_GREP_PATTERN_MAX + 1);
        big[SH_GREP_PATTERN_MAX + 1] = '\0';
        CHECK(sh_grep_compile(big, 0) == NULL);
        CHECK(errno == ERANGE);
        free(big);
    }
}

/* Patterns that make a backtracker exponential must answer SH_GREP_STEP_MAX, not park the loop. */
static void test_no_exponential_blowup(void)
{
    char line[600];
    sh_grep_t *re;
    int hit;

    memset(line, 'a', sizeof(line) - 1);
    line[sizeof(line) - 1] = '\0';

    re = sh_grep_compile("\\(a*\\)*b", 0);
    CHECK(re != NULL);
    if (re != NULL) {
        hit = sh_grep_match(re, line, 0);
        CHECK(hit == 0); /* no b anywhere: correctly no match, and it returned */
        sh_grep_free(re);
    }

    re = sh_grep_compile("\\(a*\\)*\\(a*\\)*b", 0);
    CHECK(re != NULL);
    if (re != NULL) {
        CHECK(sh_grep_match(re, line, 0) == 0);
        sh_grep_free(re);
    }

    re = sh_grep_compile("a*a*a*a*a*a*a*a*b", 0);
    CHECK(re != NULL);
    if (re != NULL) {
        CHECK(sh_grep_match(re, line, 0) == 0);
        line[sizeof(line) - 2] = 'b';
        CHECK(sh_grep_match(re, line, 0) == 1);
        sh_grep_free(re);
    }
}

static void test_bounds_stated(void)
{
    /* The line bound is grep's contract with the user, so it must be a holdable number. */
    CHECK(SH_GREP_LINE_MAX >= 256);
    CHECK(SH_GREP_PATTERN_MAX >= 16);
    CHECK(SH_GREP_SYNTAX[0] != '\0');
    /* The syntax string has to claim the engine that shipped. */
    CHECK(strstr(SH_GREP_SYNTAX, "basic") != NULL);
}

static void test_literal_search(void)
{
    const char *hay = "boot ux0:app/syslog";

    CHECK(sh_grep_find(hay, "ux0", 3, 0) == hay + 5);
    CHECK(sh_grep_find(hay, "UX0", 3, 0) == NULL);
    CHECK(sh_grep_find(hay, "UX0", 3, 1) == hay + 5);
    CHECK(sh_grep_find(hay, "log", 3, 1) == hay + 16);
    CHECK(sh_grep_find(hay, "zzz", 3, 0) == NULL);
    CHECK(sh_grep_find(hay, "", 0, 0) == hay);
    /* A partial run has to restart rather than be consumed by the comparison. */
    CHECK(sh_grep_find("aab", "ab", 2, 0) == &"aab"[1]);
    CHECK(sh_grep_find("aAaA", "AA", 2, 1) == &"aAaA"[0]);
    CHECK(sh_grep_find("aAaA", "AA", 2, 0) == NULL);
    CHECK(sh_grep_fold('A') == 'a');
    CHECK(sh_grep_fold('z') == 'z');
    CHECK(sh_grep_fold('0') == '0');
    CHECK(sh_grep_fold('\xc1') == '\xc1');
}

/* A literal pattern takes the substring path and a metacharacter one the graph; both must agree. */
static void test_literal_and_engine_agree(void)
{
    static const char *const lines[] = {
        "ux0", "app/ux0/log", "", "UX0", "a.c", "aaa", "a b c", "boot"
    };
    static const char *const pats[] = { "ux0", "a.c", "log", "", "aaa" };
    size_t i, j;

    for (i = 0; i < sizeof(pats) / sizeof(pats[0]); i++) {
        for (j = 0; j < sizeof(lines) / sizeof(lines[0]); j++) {
            sh_grep_t *re = sh_grep_compile(pats[i], 0);
            int via_engine;

            CHECK(re != NULL);
            if (re == NULL) {
                continue;
            }
            via_engine = sh_grep_match(re, lines[j], 0);
            /* Second call reuses the scratch set per line; stale state would misanswer. */
            CHECK(sh_grep_match(re, lines[j], 0) == via_engine);
            sh_grep_free(re);
        }
    }
}

/* A byte source over a fixed buffer, for the line reader. */
typedef struct {
    const char *p;
    size_t left;
} mem_src_t;

static size_t mem_fill(void *ctx, char *p, size_t want)
{
    mem_src_t *m = ctx;
    size_t n = (want < m->left) ? want : m->left;

    memcpy(p, m->p, n);
    m->p += n;
    m->left -= n;
    return n;
}

/* The bounded line reader: newline split, end of input, no final newline, and the overlong line. */
static void test_read_line(void)
{
    char line[8];
    mem_src_t m;
    int eof;

    m.p = "ab\ncd\n";
    m.left = 5;
    eof = 0;
    CHECK(sh_grep_read_line(mem_fill, &m, line, sizeof(line), &eof) == 2);
    CHECK(strcmp(line, "ab") == 0 && !eof);
    CHECK(sh_grep_read_line(mem_fill, &m, line, sizeof(line), &eof) == 2);
    CHECK(strcmp(line, "cd") == 0 && !eof);
    CHECK(sh_grep_read_line(mem_fill, &m, line, sizeof(line), &eof) == 0);
    CHECK(eof);

    /* A last line with no trailing newline is still a line, and eof only comes after. */
    m.p = "tail";
    m.left = 4;
    eof = 0;
    CHECK(sh_grep_read_line(mem_fill, &m, line, sizeof(line), &eof) == 4);
    CHECK(strcmp(line, "tail") == 0 && !eof);
    CHECK(sh_grep_read_line(mem_fill, &m, line, sizeof(line), &eof) == 0);
    CHECK(eof);

    /* A line that reaches the cap is refused (-1), not truncated, and drained to the next line. */
    m.p = "1234567890\nok\n";
    m.left = 13;
    eof = 0;
    CHECK(sh_grep_read_line(mem_fill, &m, line, sizeof(line), &eof) == -1);
    CHECK(!eof);
    CHECK(sh_grep_read_line(mem_fill, &m, line, sizeof(line), &eof) == 2);
    CHECK(strcmp(line, "ok") == 0);

    /* Blank lines are lines, not end of input: eof stays clear across them. */
    m.p = "\n\n";
    m.left = 2;
    eof = 0;
    CHECK(sh_grep_read_line(mem_fill, &m, line, sizeof(line), &eof) == 0);
    CHECK(!eof);
    CHECK(sh_grep_read_line(mem_fill, &m, line, sizeof(line), &eof) == 0);
    CHECK(!eof);
    CHECK(sh_grep_read_line(mem_fill, &m, line, sizeof(line), &eof) == 0);
    CHECK(eof);
}

/* Compiles, finds the leftmost-longest match at or after from; returns 1 and the span, 0 for none. */
static int span(const char *pattern, int flags, const char *line, size_t from, size_t *so,
                size_t *eo)
{
    sh_grep_t *re = sh_grep_compile(pattern, flags);
    int r;

    if (re == NULL) {
        return -2;
    }
    r = sh_grep_span(re, line, from, so, eo);
    sh_grep_free(re);
    return r;
}

/* "^$" is a blank line, not every line: grep -v '^$' must keep the lines that have text. */
static void test_empty_line_anchor(void)
{
    CHECK(run("^$", 0, "", 0) == 1);
    CHECK(run("^$", 0, "abc", 0) == 0);
    CHECK(run("^$", 0, "abc", 1) == 1);
    /* An anchored body that needs a byte must not match the empty line. */
    CHECK(run("^a$", 0, "", 0) == 0);
    CHECK(run("^\\(ab\\)$", 0, "", 0) == 0);
    CHECK(run("^a*$", 0, "", 0) == 1);
    CHECK(run("^", 0, "abc", 0) == 1);
    CHECK(run("$", 0, "abc", 0) == 1);
}

static void test_span(void)
{
    size_t so = 99, eo = 99;

    /* Literal path: the first occurrence, then the next one from a later start. */
    CHECK(span("ab", 0, "xabyab", 0, &so, &eo) == 1 && so == 1 && eo == 3);
    CHECK(span("ab", 0, "xabyab", 2, &so, &eo) == 1 && so == 4 && eo == 6);
    CHECK(span("ab", 0, "xabyab", 5, &so, &eo) == 0);
    CHECK(span("AB", SH_GREP_ICASE, "xabyab", 0, &so, &eo) == 1 && so == 1 && eo == 3);

    /* Leftmost wins over longest; among starts at one position the longest wins. */
    CHECK(span("a*", 0, "baaac", 0, &so, &eo) == 1 && so == 0 && eo == 0);
    CHECK(span("a\\+", 0, "baaac", 0, &so, &eo) == 1 && so == 1 && eo == 4);
    CHECK(span("a\\|ab", 0, "xabx", 0, &so, &eo) == 1 && so == 1 && eo == 3);
    CHECK(span("[0-9]\\{2,3\\}", 0, "a12345", 0, &so, &eo) == 1 && so == 1 && eo == 4);
    CHECK(span("x.*y", 0, "axbyby", 0, &so, &eo) == 1 && so == 1 && eo == 6);

    /* Anchors use the whole line, not the start offset. */
    CHECK(span("^a", 0, "aaa", 0, &so, &eo) == 1 && so == 0 && eo == 1);
    CHECK(span("^a", 0, "aaa", 1, &so, &eo) == 0);
    CHECK(span("a$", 0, "aaa", 0, &so, &eo) == 1 && so == 2 && eo == 3);
    CHECK(span("$", 0, "abc", 0, &so, &eo) == 1 && so == 3 && eo == 3);
    CHECK(span("^$", 0, "", 0, &so, &eo) == 1 && so == 0 && eo == 0);

    /* The empty pattern matches the empty string at the start offset. */
    CHECK(span("", 0, "abc", 2, &so, &eo) == 1 && so == 2 && eo == 2);
    /* An offset at the end of the line can still match empty, and past it cannot. */
    CHECK(span("x*", 0, "abc", 3, &so, &eo) == 1 && so == 3 && eo == 3);
    CHECK(span("x*", 0, "abc", 4, &so, &eo) == 0);
    CHECK(span("x", 0, "abc", 0, &so, &eo) == 0);
}

/* Whole-string match of pat on s[b..e), done through the plain matcher on a copy. */
static int whole(const char *pat, const char *s, size_t b, size_t e)
{
    char wrapped[64], sub[16];
    int r;

    snprintf(wrapped, sizeof(wrapped), "^\\(%s\\)$", pat);
    memcpy(sub, s + b, e - b);
    sub[e - b] = '\0';
    r = run(wrapped, 0, sub, 0);
    return r == 1;
}

/* Every span is checked against brute force: leftmost start, then longest end, over all strings. */
static void test_span_sweep(void)
{
    static const char *pats[] = {"a", "ab", "a*", "a\\+b", "\\(ab\\)*", "a\\|ab", "b*a",
                                 ".", "a.b", "[ab]\\{2\\}", "a*b*", "\\(a\\|b\\)\\+"};
    char s[8];
    size_t p;

    for (p = 0; p < sizeof(pats) / sizeof(pats[0]); p++) {
        int len, code;

        for (len = 0; len <= 5; len++) {
            for (code = 0; code < (1 << len); code++) {
                size_t from, b, e, bso = 0, beo = 0;
                int found;
                int i;

                for (i = 0; i < len; i++) {
                    s[i] = (code >> i) & 1 ? 'b' : 'a';
                }
                s[len] = '\0';
                for (from = 0; from <= (size_t)len; from++) {
                    size_t so = 99, eo = 99;
                    int got = span(pats[p], 0, s, from, &so, &eo);

                    found = 0;
                    for (b = from; b <= (size_t)len && !found; b++) {
                        for (e = (size_t)len + 1; e-- > b;) {
                            if (whole(pats[p], s, b, e)) {
                                found = 1;
                                bso = b;
                                beo = e;
                                break;
                            }
                        }
                    }
                    CHECK(got == found);
                    if (got != found) {
                        printf("  pat %s line '%s' from %zu: span %d model %d\n", pats[p], s,
                               from, got, found);
                    }
                    if (got == 1 && found) {
                        CHECK(so == bso && eo == beo);
                        if (so != bso || eo != beo) {
                            printf("  pat %s line %s from %zu: got %zu-%zu want %zu-%zu\n",
                                   pats[p], s, from, so, eo, bso, beo);
                        }
                    }
                }
            }
        }
    }
}

int main(void)
{
    test_bre_anchors_and_dots();
    test_bre_quantifiers();
    test_bre_classes_and_alternation();
    test_bre_literals();
    test_icase();
    test_invert();
    test_bad_patterns();
    test_no_exponential_blowup();
    test_bounds_stated();
    test_literal_search();
    test_literal_and_engine_agree();
    test_read_line();
    test_empty_line_anchor();
    test_span();
    test_span_sweep();

    if (failures == 0) {
        printf("PASS sh_grep\n");
    }
    return failures != 0;
}
