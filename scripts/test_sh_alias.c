/* Host test for src/sh_alias.c; driven by scripts/test_sh_alias.py. */
#include "sh_alias.h"

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

/* True when set fails with exactly want. */
static int set_fails(sh_alias_table_t *t, const char *name, const char *value, int want)
{
    errno = 0;
    return sh_alias_set(t, name, value) == -1 && errno == want;
}

static void test_names(void)
{
    char longname[SH_ALIAS_NAME_MAX + 2];

    CHECK(sh_alias_name_ok("l"));
    CHECK(sh_alias_name_ok("ll"));
    CHECK(sh_alias_name_ok(".."));
    CHECK(sh_alias_name_ok("..."));
    CHECK(sh_alias_name_ok("my-alias_2.x"));
    CHECK(sh_alias_name_ok("a+b,c:d@e%f^g"));

    CHECK(!sh_alias_name_ok(""));
    CHECK(!sh_alias_name_ok("a b"));
    CHECK(!sh_alias_name_ok("a\tb"));
    CHECK(!sh_alias_name_ok("a=b"));
    CHECK(!sh_alias_name_ok("a/b"));
    CHECK(!sh_alias_name_ok("a$"));
    CHECK(!sh_alias_name_ok("a'"));
    CHECK(!sh_alias_name_ok("a\""));
    CHECK(!sh_alias_name_ok("a\\"));
    CHECK(!sh_alias_name_ok("a|b"));
    CHECK(!sh_alias_name_ok("a;b"));
    CHECK(!sh_alias_name_ok("a&b"));
    CHECK(!sh_alias_name_ok("a>b"));
    CHECK(!sh_alias_name_ok("a*"));
    CHECK(!sh_alias_name_ok("a?"));
    CHECK(!sh_alias_name_ok("a["));
    CHECK(!sh_alias_name_ok(NULL));

    memset(longname, 'x', SH_ALIAS_NAME_MAX);
    longname[SH_ALIAS_NAME_MAX] = '\0';
    CHECK(sh_alias_name_ok(longname));
    longname[SH_ALIAS_NAME_MAX] = 'x';
    longname[SH_ALIAS_NAME_MAX + 1] = '\0';
    CHECK(!sh_alias_name_ok(longname));
}

static void test_seed(void)
{
    sh_alias_table_t *t = sh_alias_new();

    CHECK(t != NULL);
    CHECK(sh_alias_count(t) == 0);
    CHECK(sh_alias_seed(t) == 0);

    /* AC2: l, ll, la and the two directory shortcuts. */
    CHECK(strcmp(sh_alias_get(t, "l"), "ls -l") == 0);
    CHECK(strcmp(sh_alias_get(t, "ll"), "ls -la") == 0);
    CHECK(strcmp(sh_alias_get(t, "la"), "ls -a") == 0);
    CHECK(strcmp(sh_alias_get(t, ".."), "cd ..") == 0);
    CHECK(strcmp(sh_alias_get(t, "..."), "cd ../..") == 0);
    CHECK(sh_alias_count(t) == 5);

    /* Listing is in name order. */
    CHECK(strcmp(sh_alias_name_at(t, 0), "..") == 0);
    CHECK(strcmp(sh_alias_name_at(t, 1), "...") == 0);
    CHECK(strcmp(sh_alias_name_at(t, 2), "l") == 0);
    CHECK(strcmp(sh_alias_name_at(t, 3), "la") == 0);
    CHECK(strcmp(sh_alias_name_at(t, 4), "ll") == 0);
    CHECK(sh_alias_name_at(t, 5) == NULL);
    CHECK(sh_alias_value_at(t, 5) == NULL);
    CHECK(sh_alias_name_at(t, -1) == NULL);
    sh_alias_free(t);
}

static void test_define_replace_remove(void)
{
    sh_alias_table_t *t = sh_alias_new();

    /* AC1 and AC5: a value with spaces is kept whole. */
    CHECK(sh_alias_set(t, "two", "echo a b") == 0);
    CHECK(strcmp(sh_alias_get(t, "two"), "echo a b") == 0);
    CHECK(sh_alias_count(t) == 1);

    /* Setting an existing name replaces it and adds nothing. */
    CHECK(sh_alias_set(t, "two", "echo c") == 0);
    CHECK(strcmp(sh_alias_get(t, "two"), "echo c") == 0);
    CHECK(sh_alias_count(t) == 1);

    /* unalias of a name never defined is ENOENT. */
    CHECK(sh_alias_unset(t, "nope") == -1 && errno == ENOENT);
    CHECK(sh_alias_unset(t, "two") == 0);
    CHECK(sh_alias_get(t, "two") == NULL);
    CHECK(sh_alias_count(t) == 0);
    CHECK(sh_alias_unset(t, "two") == -1 && errno == ENOENT);

    /* A lookup of a name that is not there, an argument that merely resembles one, or a NULL table. */
    CHECK(sh_alias_get(t, "ll") == NULL);
    CHECK(sh_alias_get(NULL, "ll") == NULL);
    CHECK(sh_alias_count(NULL) == 0);
    CHECK(set_fails(NULL, "a", "x", EINVAL));
    sh_alias_free(t);
    sh_alias_free(NULL);
}

static void test_limits(void)
{
    sh_alias_table_t *t = sh_alias_new();
    char name[SH_ALIAS_NAME_MAX + 2], value[SH_ALIAS_VALUE_MAX + 2];
    int i;

    CHECK(set_fails(t, "", "x", EINVAL));
    CHECK(set_fails(t, "a b", "x", EINVAL));
    CHECK(set_fails(t, "a", "", EINVAL));
    CHECK(set_fails(t, "a", " ", EINVAL)); /* a value of only blanks would expand to nothing */
    CHECK(set_fails(t, "a", " \t ", EINVAL));
    CHECK(sh_alias_set(t, "a", " x") == 0);

    memset(name, 'n', SH_ALIAS_NAME_MAX + 1);
    name[SH_ALIAS_NAME_MAX + 1] = '\0';
    CHECK(set_fails(t, name, "x", ENAMETOOLONG) || set_fails(t, name, "x", EINVAL));

    memset(value, 'v', SH_ALIAS_VALUE_MAX);
    value[SH_ALIAS_VALUE_MAX] = '\0';
    CHECK(sh_alias_set(t, "big", value) == 0);
    CHECK(strcmp(sh_alias_get(t, "big"), value) == 0);
    value[SH_ALIAS_VALUE_MAX] = 'v';
    value[SH_ALIAS_VALUE_MAX + 1] = '\0';
    CHECK(set_fails(t, "big2", value, ENAMETOOLONG));
    CHECK(sh_alias_get(t, "big2") == NULL);
    CHECK(strlen(sh_alias_get(t, "big")) == SH_ALIAS_VALUE_MAX); /* the refused set changed nothing */

    /* AC: the table holds SH_ALIAS_MAX and refuses the next new name, but still replaces and removes. */
    sh_alias_free(t);
    t = sh_alias_new();
    for (i = 0; i < SH_ALIAS_MAX; i++) {
        snprintf(name, sizeof(name), "n%d", i);
        CHECK(sh_alias_set(t, name, "echo x") == 0);
    }
    CHECK(sh_alias_count(t) == SH_ALIAS_MAX);
    CHECK(set_fails(t, "extra", "echo x", ENOSPC));
    CHECK(sh_alias_get(t, "extra") == NULL);
    CHECK(sh_alias_set(t, "n0", "echo y") == 0);
    CHECK(strcmp(sh_alias_get(t, "n0"), "echo y") == 0);
    CHECK(sh_alias_unset(t, "n0") == 0);
    CHECK(sh_alias_set(t, "extra", "echo x") == 0);
    sh_alias_free(t);
}

/* The values sh_alias_chain returns, joined with '|', or "" when none. */
static const char *chain_str(const sh_alias_table_t *t, const char *word, int max)
{
    static char buf[1024];
    const char *vals[SH_ALIAS_MAX + 1];
    int i, n = sh_alias_chain(t, word, vals, max);

    buf[0] = '\0';
    for (i = 0; i < n; i++) {
        if (i > 0) {
            strcat(buf, "|");
        }
        strcat(buf, vals[i]);
    }
    return buf;
}

/* AC10: an alias expands through other aliases, each at most once, as in bash. */
static void test_chain(void)
{
    sh_alias_table_t *t = sh_alias_new();

    CHECK(sh_alias_seed(t) == 0);

    /* One alias, then the first word of its value is not an alias. */
    CHECK(strcmp(chain_str(t, "l", SH_ALIAS_MAX), "ls -l") == 0);
    CHECK(strcmp(chain_str(t, "nope", SH_ALIAS_MAX), "") == 0);
    CHECK(strcmp(chain_str(t, "", SH_ALIAS_MAX), "") == 0);

    /* alias l=ll with ll defined: l runs through ll. */
    CHECK(sh_alias_set(t, "l", "ll") == 0);
    CHECK(strcmp(chain_str(t, "l", SH_ALIAS_MAX), "ll|ls -la") == 0);

    /* A value starting with its own name stops there, so alias ls='ls -l' reaches the builtin. */
    CHECK(sh_alias_set(t, "ls", "ls -l") == 0);
    CHECK(strcmp(chain_str(t, "ls", SH_ALIAS_MAX), "ls -l") == 0);
    CHECK(sh_alias_set(t, "mine", "ls") == 0);
    CHECK(strcmp(chain_str(t, "mine", SH_ALIAS_MAX), "ls|ls -l") == 0);

    /* An alias already applied is left alone when it comes back around. */
    CHECK(sh_alias_set(t, "b", "b -y") == 0);
    CHECK(sh_alias_set(t, "a", "b x") == 0);
    CHECK(strcmp(chain_str(t, "a", SH_ALIAS_MAX), "b x|b -y") == 0);

    /* Only the first word counts, leading blanks and tabs do not hide it, a quoted word is plain. ll leads
     * to ls here, which was defined above as an alias of its own. */
    CHECK(sh_alias_set(t, "w1", "echo ll") == 0);
    CHECK(strcmp(chain_str(t, "w1", SH_ALIAS_MAX), "echo ll") == 0);
    CHECK(sh_alias_set(t, "w2", "  ll -x") == 0);
    CHECK(strcmp(chain_str(t, "w2", SH_ALIAS_MAX), "  ll -x|ls -la|ls -l") == 0);
    CHECK(sh_alias_set(t, "w3", "ll\t-x") == 0);
    CHECK(strcmp(chain_str(t, "w3", SH_ALIAS_MAX), "ll\t-x|ls -la|ls -l") == 0);
    CHECK(sh_alias_set(t, "q1", "'ll' -x") == 0);
    CHECK(strcmp(chain_str(t, "q1", SH_ALIAS_MAX), "'ll' -x") == 0);
    CHECK(sh_alias_set(t, "q2", "\\ll") == 0);
    CHECK(strcmp(chain_str(t, "q2", SH_ALIAS_MAX), "\\ll") == 0);

    /* max caps the walk; a word longer than any name has no alias. */
    CHECK(strcmp(chain_str(t, "mine", 1), "ls") == 0);
    CHECK(strcmp(chain_str(t, "mine", 0), "") == 0);
    {
        char longname[SH_ALIAS_NAME_MAX + 8];

        memset(longname, 'k', sizeof(longname) - 1);
        longname[sizeof(longname) - 1] = '\0';
        CHECK(strcmp(chain_str(t, longname, SH_ALIAS_MAX), "") == 0);
    }
    CHECK(sh_alias_chain(NULL, "l", NULL, 0) == 0);
    sh_alias_free(t);
}

/* AC11: a definition that would close a ring of aliases is refused; naming itself is not a ring. */
static void test_rings(void)
{
    sh_alias_table_t *t = sh_alias_new();

    CHECK(sh_alias_seed(t) == 0);
    CHECK(sh_alias_set(t, "l", "ll") == 0);

    /* ll=l would make l -> ll -> l: refused, and ll keeps its value. */
    CHECK(set_fails(t, "ll", "l", ELOOP));
    CHECK(set_fails(t, "ll", "l -h", ELOOP));
    CHECK(strcmp(sh_alias_get(t, "ll"), "ls -la") == 0);
    CHECK(sh_alias_count(t) == 5);

    /* A longer ring. */
    CHECK(sh_alias_set(t, "r1", "r2") == 0);
    CHECK(sh_alias_set(t, "r2", "r3") == 0);
    CHECK(set_fails(t, "r3", "r1", ELOOP));
    CHECK(sh_alias_get(t, "r3") == NULL);
    CHECK(sh_alias_set(t, "r3", "echo done") == 0);
    CHECK(set_fails(t, "r3", "r1 again", ELOOP)); /* replacing an existing value is checked too */
    CHECK(strcmp(sh_alias_get(t, "r3"), "echo done") == 0);

    /* Naming itself, a builtin, an unknown word or a quoted word is fine. */
    CHECK(sh_alias_set(t, "ls", "ls -l") == 0);
    CHECK(sh_alias_set(t, "g", "grep -n") == 0);
    CHECK(sh_alias_set(t, "later", "notdefinedyet") == 0);
    CHECK(sh_alias_set(t, "x1", "'x1'") == 0);

    /* Leading blanks and a tab after the first word do not hide a ring. */
    CHECK(sh_alias_set(t, "p", "echo") == 0);
    CHECK(sh_alias_set(t, "q", "p") == 0);
    CHECK(set_fails(t, "p", "  q", ELOOP));
    CHECK(set_fails(t, "p", "q\t-x", ELOOP));

    /* A first word of exactly the longest name is still compared. */
    {
        char longname[SH_ALIAS_NAME_MAX + 8];

        memset(longname, 'k', SH_ALIAS_NAME_MAX);
        longname[SH_ALIAS_NAME_MAX] = '\0';
        CHECK(sh_alias_set(t, longname, "echo k") == 0);
        strcat(longname, " -x");
        CHECK(sh_alias_set(t, "kk", longname) == 0); /* an alias, so kk follows it: no ring */
        CHECK(sh_alias_chain(t, "kk", (const char *[SH_ALIAS_MAX]){0}, SH_ALIAS_MAX) == 2);
    }
    sh_alias_free(t);
}

/* Every operation sequence up to length 4 over three names and five values, against a model. */
static const char *names[] = {"a", "b", "c"};
static const char *values[] = {"x", "a", "b", "c", "x y"};
#define NNAMES 3
#define NVALUES 5
#define NOPS (NNAMES * NVALUES + NNAMES)

typedef struct {
    int has[NNAMES];
    const char *val[NNAMES];
} model_t;

/* Index of the name a value begins with, -1 when its first word is not one of the model's names. */
static int first_name(const model_t *m, const char *value)
{
    int i;
    size_t len = strcspn(value, " \t");

    for (i = 0; i < NNAMES; i++) {
        if (m->has[i] && strlen(names[i]) == len && strncmp(names[i], value, len) == 0) {
            return i;
        }
    }
    return -1;
}

/* The ring rule, as a walk: from the first word of the new value, does it come back to self past step 0. */
static int model_ring(const model_t *m, int self, const char *value)
{
    int visited[NNAMES] = {0}, step = 0;
    size_t len = strcspn(value, " \t");
    const char *cur = value;

    for (;;) {
        int at;

        if (strlen(names[self]) == len && strncmp(names[self], cur, len) == 0) {
            return step > 0;
        }
        at = first_name(m, cur);
        if (at < 0 || visited[at]) {
            return 0;
        }
        visited[at] = 1;
        cur = m->val[at];
        len = strcspn(cur, " \t");
        step++;
    }
}

/* The expected chain for a name, joined with '|'. */
static void model_chain(const model_t *m, int n, char *out)
{
    int applied[NNAMES] = {0};
    const char *cur = names[n];

    out[0] = '\0';
    for (;;) {
        int at = first_name(m, cur);

        if (at < 0 || applied[at]) {
            return;
        }
        applied[at] = 1;
        if (out[0] != '\0') {
            strcat(out, "|");
        }
        strcat(out, m->val[at]);
        cur = m->val[at];
    }
}

static void check_same(const sh_alias_table_t *t, const model_t *m, const char *trace)
{
    int i, listed = 0;

    for (i = 0; i < NNAMES; i++) {
        const char *got = sh_alias_get(t, names[i]);
        char want[256];

        if (m->has[i] ? (got == NULL || strcmp(got, m->val[i]) != 0) : got != NULL) {
            printf("FAIL model get %s after %s\n", names[i], trace);
            failures++;
        }
        listed += m->has[i];
        model_chain(m, i, want);
        if (strcmp(chain_str(t, names[i], SH_ALIAS_MAX), want) != 0) {
            printf("FAIL model chain %s after %s: got \"%s\" want \"%s\"\n", names[i], trace,
                   chain_str(t, names[i], SH_ALIAS_MAX), want);
            failures++;
        }
    }
    if (sh_alias_count(t) != listed) {
        printf("FAIL model count after %s\n", trace);
        failures++;
    }
    /* Listing order is name order, and names a < b < c. */
    for (i = 0; i < listed; i++) {
        int k, seen = -1;

        for (k = 0; k < NNAMES; k++) {
            if (m->has[k] && ++seen == i) {
                break;
            }
        }
        if (strcmp(sh_alias_name_at(t, i), names[k]) != 0 || strcmp(sh_alias_value_at(t, i), m->val[k]) != 0) {
            printf("FAIL model order at %d after %s\n", i, trace);
            failures++;
        }
    }
}

static void test_model(void)
{
    int len;

    for (len = 1; len <= 4; len++) {
        long total = 1, code;
        int i;

        for (i = 0; i < len; i++) {
            total *= NOPS;
        }
        for (code = 0; code < total; code++) {
            sh_alias_table_t *t = sh_alias_new();
            model_t m;
            long c = code;
            char trace[64];

            memset(&m, 0, sizeof(m));
            trace[0] = '\0';
            for (i = 0; i < len; i++) {
                int op = (int)(c % NOPS), n, v, rc;

                c /= NOPS;
                if (op < NNAMES * NVALUES) {
                    n = op / NVALUES;
                    v = op % NVALUES;
                    snprintf(trace + strlen(trace), sizeof(trace) - strlen(trace), "set %s=%s;", names[n], values[v]);
                    rc = sh_alias_set(t, names[n], values[v]);
                    if (model_ring(&m, n, values[v])) {
                        if (rc != -1 || errno != ELOOP) {
                            printf("FAIL model want ELOOP for %s\n", trace);
                            failures++;
                        }
                    } else {
                        if (rc != 0) {
                            printf("FAIL model want success for %s\n", trace);
                            failures++;
                        }
                        m.has[n] = 1;
                        m.val[n] = values[v];
                    }
                } else {
                    n = op - NNAMES * NVALUES;
                    snprintf(trace + strlen(trace), sizeof(trace) - strlen(trace), "unset %s;", names[n]);
                    rc = sh_alias_unset(t, names[n]);
                    if (m.has[n] ? rc != 0 : (rc != -1 || errno != ENOENT)) {
                        printf("FAIL model unset result for %s\n", trace);
                        failures++;
                    }
                    m.has[n] = 0;
                }
                check_same(t, &m, trace);
            }
            sh_alias_free(t);
        }
    }
}

int main(void)
{
    test_names();
    test_seed();
    test_define_replace_remove();
    test_limits();
    test_chain();
    test_rings();
    test_model();
    if (failures != 0) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("ok\n");
    return 0;
}
