/* Host test for src/sh_expand.c; driven by scripts/test_sh_expand.py. */
#include "sh_expand.h"

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

#define VALUE_A "a*b"

static const char *lookup(void *ctx, const char *name)
{
    (void)ctx;
    if (strcmp(name, "A") == 0) {
        return VALUE_A;
    }
    if (strcmp(name, "EMPTY") == 0) {
        return "";
    }
    if (strcmp(name, "DOLLAR") == 0) {
        return "$A";
    }
    if (strcmp(name, "PLAIN") == 0) {
        return "plain";
    }
    if (strcmp(name, "SP") == 0) {
        return "a b";
    }
    if (strcmp(name, "LEAD") == 0) {
        return "  lead";
    }
    if (strcmp(name, "TRAIL") == 0) {
        return "trail  ";
    }
    if (strcmp(name, "WIDE") == 0) {
        return "a   b";
    }
    if (strcmp(name, "BLANK") == 0) {
        return " ";
    }
    if (strcmp(name, "WS") == 0) {
        return "a\tb\nc";
    }
    if (strcmp(name, "COL") == 0) {
        return "a:b::c:";
    }
    if (strcmp(name, "LCOL") == 0) {
        return ":a";
    }
    if (strcmp(name, "MIX") == 0) {
        return "a : b";
    }
    return NULL;
}

static sh_expand_status_t run_ifs(const char *word, const char *ifs, char *out, size_t cap, sh_expand_info_t *info)
{
    return sh_expand_word(word, lookup, NULL, ifs, out, cap, info);
}

static sh_expand_status_t run(const char *word, char *out, size_t cap, sh_expand_info_t *info)
{
    return run_ifs(word, NULL, out, cap, info);
}

/* The fields joined with `|`, or "ERR" when the expansion fails; nfields is checked against the count. */
static const char *fields_of(const char *word, const char *ifs)
{
    static char joined[512];
    char out[256];
    sh_expand_info_t info;
    const char *f = out;
    int i;

    if (run_ifs(word, ifs, out, sizeof(out), &info) != SH_EXPAND_OK) {
        return "ERR";
    }
    joined[0] = '\0';
    for (i = 0; i < info.nfields; i++) {
        if (i > 0) {
            strcat(joined, "|");
        }
        strcat(joined, f);
        f += strlen(f) + 1;
    }
    return joined;
}

static int expands_to(const char *word, const char *want, int refs)
{
    char out[256];
    sh_expand_info_t info;

    if (run(word, out, sizeof(out), &info) != SH_EXPAND_OK) {
        return 0;
    }
    return info.nfields == (want[0] != '\0' ? 1 : 0) && strcmp(out, want) == 0 && info.refs == refs;
}

static int fails_with(const char *word, sh_expand_status_t want, const char *what)
{
    char out[256];
    sh_expand_info_t info;

    return run(word, out, sizeof(out), &info) == want && strcmp(info.what, what) == 0;
}

static void test_basic(void)
{
    CHECK(expands_to("plain", "plain", 0));
    CHECK(expands_to("", "", 0));
    CHECK(expands_to("$PLAIN", "plain", 1));
    CHECK(expands_to("${PLAIN}", "plain", 1));
    CHECK(expands_to("x$PLAIN.y", "xplain.y", 1));
    CHECK(expands_to("x${PLAIN}y", "xplainy", 1));
    CHECK(expands_to("$PLAIN$PLAIN", "plainplain", 2));
    CHECK(expands_to("$UNSET", "", 1));
    CHECK(expands_to("a$UNSET", "a", 1));
    CHECK(expands_to("${UNSET}", "", 1));
    CHECK(expands_to("$EMPTY", "", 1));
    CHECK(expands_to("$A", VALUE_A, 1));
    CHECK(expands_to("\x02SP", "a b", 1));
    CHECK(expands_to("*$PLAIN", "*plain", 1));
}

static void test_value_not_rescanned(void)
{
    CHECK(expands_to("$DOLLAR", "$A", 1));
    CHECK(expands_to("${DOLLAR}", "$A", 1));
}

static void test_literal_dollar(void)
{
    CHECK(expands_to("\x01PLAIN", "$PLAIN", 0));
    CHECK(expands_to("\x01{PLAIN}", "${PLAIN}", 0));
    CHECK(expands_to("a\x01", "a$", 0));
    CHECK(expands_to("$", "$", 0));
    CHECK(expands_to("a$", "a$", 0));
    CHECK(expands_to("$ x", "$ x", 0));
    CHECK(expands_to("$/x", "$/x", 0));
    CHECK(expands_to("$.", "$.", 0));
    CHECK(expands_to("$=", "$=", 0));
}

static void test_unsupported(void)
{
    CHECK(fails_with("$(date)", SH_EXPAND_UNSUPPORTED, "$("));
    CHECK(fails_with("x$((1+1))", SH_EXPAND_UNSUPPORTED, "$(("));
    CHECK(fails_with("${A:-x}", SH_EXPAND_UNSUPPORTED, "${A:"));
    CHECK(fails_with("${A-x}", SH_EXPAND_UNSUPPORTED, "${A-"));
    CHECK(fails_with("${A#x}", SH_EXPAND_UNSUPPORTED, "${A#"));
    CHECK(fails_with("${A%x}", SH_EXPAND_UNSUPPORTED, "${A%"));
    CHECK(fails_with("${A/x/y}", SH_EXPAND_UNSUPPORTED, "${A/"));
    CHECK(fails_with("$?", SH_EXPAND_UNSUPPORTED, "$?"));
    CHECK(fails_with("$1", SH_EXPAND_UNSUPPORTED, "$1"));
    CHECK(fails_with("$0", SH_EXPAND_UNSUPPORTED, "$0"));
    CHECK(fails_with("$$", SH_EXPAND_UNSUPPORTED, "$$"));
    CHECK(fails_with("$#", SH_EXPAND_UNSUPPORTED, "$#"));
    CHECK(fails_with("$@", SH_EXPAND_UNSUPPORTED, "$@"));
    CHECK(fails_with("$*", SH_EXPAND_UNSUPPORTED, "$*"));
    CHECK(fails_with("$!", SH_EXPAND_UNSUPPORTED, "$!"));
    CHECK(fails_with("$-", SH_EXPAND_UNSUPPORTED, "$-"));
    CHECK(fails_with("${1}", SH_EXPAND_UNSUPPORTED, "${1"));
}

static void test_malformed(void)
{
    CHECK(fails_with("${", SH_EXPAND_MALFORMED, "${"));
    CHECK(fails_with("${}", SH_EXPAND_MALFORMED, "${}"));
    CHECK(fails_with("${A", SH_EXPAND_MALFORMED, "${A"));
    CHECK(fails_with("${A B}", SH_EXPAND_MALFORMED, "${A "));
    CHECK(fails_with("${#A}", SH_EXPAND_MALFORMED, "${#"));
    CHECK(fails_with("${ A}", SH_EXPAND_MALFORMED, "${ "));
    CHECK(fails_with("a${A", SH_EXPAND_MALFORMED, "${A"));
}

static void test_what_is_cut(void)
{
    char out[64];
    sh_expand_info_t info;
    const char *w = "${A:-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa}";

    CHECK(run(w, out, sizeof(out), &info) == SH_EXPAND_UNSUPPORTED);
    CHECK(strcmp(info.what, "${A:") == 0);
    CHECK(run("${AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA", out, sizeof(out), &info) == SH_EXPAND_MALFORMED);
    CHECK(strlen(info.what) < sizeof(info.what));
}

static void test_bounds(void)
{
    char out[8];
    sh_expand_info_t info;

    CHECK(run("abcdefg", out, sizeof(out), &info) == SH_EXPAND_OK);
    CHECK(strcmp(out, "abcdefg") == 0);
    CHECK(run("abcdefgh", out, sizeof(out), &info) == SH_EXPAND_TOOLONG);
    CHECK(run("$PLAIN", out, sizeof(out), &info) == SH_EXPAND_OK);
    CHECK(strcmp(out, "plain") == 0);
    CHECK(run("$PLAIN$PLAIN", out, sizeof(out), &info) == SH_EXPAND_TOOLONG);
    CHECK(run("x", out, 1, &info) == SH_EXPAND_TOOLONG);
    CHECK(run("", out, 1, &info) == SH_EXPAND_OK);
    CHECK(out[0] == '\0');
    CHECK(run("", NULL, 0, &info) == SH_EXPAND_TOOLONG);
    CHECK(run("$SP$SP$SP", out, sizeof(out), &info) == SH_EXPAND_TOOLONG);
}

/* Every sequence of up to five tokens, each token expanding to a known string, against that string. Tokens
 * end where the next begins, so concatenation cannot make a longer name. */
static void test_token_sweep(void)
{
    static const struct {
        const char *text, *want;
        int refs;
    } tok[] = {
        {"x", "x", 0},
        {"${A}", VALUE_A, 1},
        {"${UNSET}", "", 1},
        {"$A:", VALUE_A ":", 1},
        {"$UNSET:", ":", 1},
        {"\x01", "$", 0},
        {"$.", "$.", 0},
        {"-", "-", 0},
    };
    const int ntok = (int)(sizeof(tok) / sizeof(tok[0]));
    int len, seq;

    for (len = 0; len <= 5; len++) {
        int total = 1;

        for (seq = 0; seq < len; seq++) {
            total *= ntok;
        }
        for (seq = 0; seq < total; seq++) {
            char word[128] = "", want[256] = "", out[256];
            int refs = 0, n = seq, i;
            sh_expand_info_t info;

            for (i = 0; i < len; i++) {
                int t = n % ntok;

                n /= ntok;
                strcat(word, tok[t].text);
                strcat(want, tok[t].want);
                refs += tok[t].refs;
            }
            if (run(word, out, sizeof(out), &info) != SH_EXPAND_OK || strcmp(out, want) != 0 || info.refs != refs ||
                info.nfields != (want[0] != '\0' ? 1 : 0)) {
                printf("FAIL sweep: word \"%s\" -> \"%s\", want \"%s\"\n", word, out, want);
                failures++;
                return;
            }
        }
    }
}

/* Every string up to six bytes over a hostile alphabet: whatever comes back stays inside the buffer. */
static void test_hostile_sweep(void)
{
    static const char alpha[] = {'A', '$', '{', '}', '\x01', '\x02', ':', ' ', '1'};
    const int na = (int)sizeof(alpha);
    int len, seq;

    for (len = 0; len <= 6; len++) {
        int total = 1;

        for (seq = 0; seq < len; seq++) {
            total *= na;
        }
        for (seq = 0; seq < total; seq++) {
            char word[8], out[16];
            int n = seq, i, plain = 1;
            sh_expand_info_t info;
            sh_expand_status_t st;

            for (i = 0; i < len; i++) {
                word[i] = alpha[n % na];
                n /= na;
                if (word[i] == '$' || word[i] == '\x01' || word[i] == '\x02') {
                    plain = 0;
                }
            }
            word[len] = '\0';
            memset(out, 'Z', sizeof(out));
            st = run(word, out, sizeof(out), &info);
            if (st == SH_EXPAND_OK) {
                CHECK(memchr(out, '\0', sizeof(out)) != NULL);
                if (plain) {
                    CHECK(strcmp(out, word) == 0 && info.refs == 0);
                }
            } else {
                CHECK(st != SH_EXPAND_UNSUPPORTED || memchr(info.what, '\0', sizeof(info.what)) != NULL);
                CHECK(!plain);
            }
        }
    }
}

static void test_split_default(void)
{
    CHECK(strcmp(fields_of("$SP", NULL), "a|b") == 0);
    CHECK(strcmp(fields_of("\x02SP", NULL), "a b") == 0);
    CHECK(strcmp(fields_of("x$SP", NULL), "xa|b") == 0);
    CHECK(strcmp(fields_of("$SP.y", NULL), "a|b.y") == 0);
    CHECK(strcmp(fields_of("$SP$SP", NULL), "a|ba|b") == 0);
    CHECK(strcmp(fields_of("$LEAD", NULL), "lead") == 0);
    CHECK(strcmp(fields_of("$TRAIL", NULL), "trail") == 0);
    CHECK(strcmp(fields_of("x$TRAIL", NULL), "xtrail") == 0);
    CHECK(strcmp(fields_of("$TRAIL.y", NULL), "trail|.y") == 0);
    CHECK(strcmp(fields_of("$WIDE", NULL), "a|b") == 0);
    CHECK(strcmp(fields_of("$BLANK", NULL), "") == 0);
    CHECK(strcmp(fields_of("$EMPTY", NULL), "") == 0);
    CHECK(strcmp(fields_of("$UNSET", NULL), "") == 0);
    CHECK(strcmp(fields_of("$WS", NULL), "a|b|c") == 0);
    CHECK(strcmp(fields_of("a b", NULL), "a b") == 0);
    CHECK(strcmp(fields_of("\x02" "EMPTY", NULL), "") == 0);
    CHECK(strcmp(fields_of("\x02" "SP$SP", NULL), "a ba|b") == 0);
}

static void test_split_counts(void)
{
    char out[64];
    sh_expand_info_t info;

    CHECK(run("$SP", out, sizeof(out), &info) == SH_EXPAND_OK && info.nfields == 2);
    CHECK(run("$BLANK", out, sizeof(out), &info) == SH_EXPAND_OK && info.nfields == 0);
    CHECK(run("$EMPTY", out, sizeof(out), &info) == SH_EXPAND_OK && info.nfields == 0);
    CHECK(run("\x02" "EMPTY", out, sizeof(out), &info) == SH_EXPAND_OK && info.nfields == 1 && out[0] == '\0');
    CHECK(run("\x02" "UNSET", out, sizeof(out), &info) == SH_EXPAND_OK && info.nfields == 1 && out[0] == '\0');
    CHECK(run("", out, sizeof(out), &info) == SH_EXPAND_OK && info.nfields == 0);
}

static void test_split_ifs(void)
{
    CHECK(strcmp(fields_of("$COL", ":"), "a|b||c") == 0);
    CHECK(strcmp(fields_of("$LCOL", ":"), "|a") == 0);
    CHECK(strcmp(fields_of("$SP", ":"), "a b") == 0);
    CHECK(strcmp(fields_of("$SP", ""), "a b") == 0);
    CHECK(strcmp(fields_of("$WIDE", ""), "a   b") == 0);
    CHECK(strcmp(fields_of("$EMPTY", ""), "") == 0);
    CHECK(strcmp(fields_of("$MIX", " :"), "a|b") == 0);
    CHECK(strcmp(fields_of("$MIX", ":"), "a | b") == 0);
    CHECK(strcmp(fields_of("a:b$PLAIN", ":"), "a:bplain") == 0);
    CHECK(strcmp(fields_of("$WS", "\n"), "a\tb|c") == 0);
    CHECK(strcmp(fields_of("$LEAD", " "), "lead") == 0);
    CHECK(strcmp(fields_of("\x02" "COL", ":"), "a:b::c:") == 0);
}

int main(void)
{
    test_basic();
    test_value_not_rescanned();
    test_literal_dollar();
    test_unsupported();
    test_malformed();
    test_what_is_cut();
    test_bounds();
    test_split_default();
    test_split_counts();
    test_split_ifs();
    test_token_sweep();
    test_hostile_sweep();
    if (failures == 0) {
        printf("sh_expand: all checks passed\n");
    }
    return failures == 0 ? 0 : 1;
}
