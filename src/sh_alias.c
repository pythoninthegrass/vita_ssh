/* Per-session shell aliases; see include/sh_alias.h. */
#include "sh_alias.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

struct sh_alias_table {
    int count;
    struct {
        char name[SH_ALIAS_NAME_MAX + 1];
        char value[SH_ALIAS_VALUE_MAX + 1];
    } e[SH_ALIAS_MAX]; /* sorted by name */
};

sh_alias_table_t *sh_alias_new(void)
{
    return calloc(1, sizeof(struct sh_alias_table));
}

void sh_alias_free(sh_alias_table_t *t)
{
    free(t);
}

int sh_alias_name_ok(const char *name)
{
    size_t i, len;

    if (name == NULL) {
        return 0;
    }
    len = strlen(name);
    if (len == 0 || len > SH_ALIAS_NAME_MAX) {
        return 0;
    }
    for (i = 0; i < len; i++) {
        char c = name[i];

        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              strchr("_.-+,:@%^", c) != NULL)) {
            return 0;
        }
    }
    return 1;
}

/* Index of name, or the index it would be inserted at as -(i + 1). */
static int find(const sh_alias_table_t *t, const char *name)
{
    int lo = 0, hi = t->count - 1;

    while (lo <= hi) {
        int mid = (lo + hi) / 2, cmp = strcmp(t->e[mid].name, name);

        if (cmp == 0) {
            return mid;
        }
        if (cmp < 0) {
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    return -(lo + 1);
}

/* Copies value's first blank-delimited word into word; 0 when it is too long to be a name. */
static int first_word(const char *value, char *word)
{
    size_t len;

    value += strspn(value, " \t");
    len = strcspn(value, " \t");
    if (len > SH_ALIAS_NAME_MAX) {
        return 0;
    }
    memcpy(word, value, len);
    word[len] = '\0';
    return 1;
}

/* Whether defining self as value would close a ring: following first words through the table from
 * value comes back to self after at least one other alias. A value that starts with self is not one. */
static int closes_ring(const sh_alias_table_t *t, const char *self, const char *value)
{
    char cur[SH_ALIAS_NAME_MAX + 1];
    int visited[SH_ALIAS_MAX] = {0};
    int step = 0;

    if (!first_word(value, cur)) {
        return 0;
    }
    for (;;) {
        int at;

        if (strcmp(cur, self) == 0) {
            return step > 0;
        }
        at = find(t, cur);
        if (at < 0 || visited[at] || !first_word(t->e[at].value, cur)) {
            return 0;
        }
        visited[at] = 1;
        step++;
    }
}

int sh_alias_chain(const sh_alias_table_t *t, const char *word, const char *out[], int max)
{
    char cur[SH_ALIAS_NAME_MAX + 1];
    int applied[SH_ALIAS_MAX] = {0};
    int n = 0;

    if (t == NULL || word == NULL || strlen(word) > SH_ALIAS_NAME_MAX) {
        return 0;
    }
    strcpy(cur, word);
    while (n < max) {
        int at = find(t, cur);

        if (at < 0 || applied[at]) {
            break;
        }
        applied[at] = 1;
        out[n++] = t->e[at].value;
        if (!first_word(t->e[at].value, cur)) {
            break;
        }
    }
    return n;
}

int sh_alias_set(sh_alias_table_t *t, const char *name, const char *value)
{
    int at;

    if (t == NULL || name == NULL || value == NULL || value[strspn(value, " \t")] == '\0') {
        errno = EINVAL;
        return -1;
    }
    if (strlen(name) > SH_ALIAS_NAME_MAX) {
        errno = ENAMETOOLONG;
        return -1;
    }
    if (!sh_alias_name_ok(name)) {
        errno = EINVAL;
        return -1;
    }
    if (strlen(value) > SH_ALIAS_VALUE_MAX) {
        errno = ENAMETOOLONG;
        return -1;
    }
    if (closes_ring(t, name, value)) {
        errno = ELOOP;
        return -1;
    }
    at = find(t, name);
    if (at < 0) {
        int i;

        if (t->count == SH_ALIAS_MAX) {
            errno = ENOSPC;
            return -1;
        }
        at = -at - 1;
        for (i = t->count; i > at; i--) {
            t->e[i] = t->e[i - 1];
        }
        t->count++;
        strcpy(t->e[at].name, name);
    }
    strcpy(t->e[at].value, value);
    return 0;
}

int sh_alias_unset(sh_alias_table_t *t, const char *name)
{
    int at, i;

    if (t == NULL || name == NULL || (at = find(t, name)) < 0) {
        errno = ENOENT;
        return -1;
    }
    for (i = at; i < t->count - 1; i++) {
        t->e[i] = t->e[i + 1];
    }
    t->count--;
    return 0;
}

const char *sh_alias_get(const sh_alias_table_t *t, const char *name)
{
    int at;

    if (t == NULL || name == NULL || (at = find(t, name)) < 0) {
        return NULL;
    }
    return t->e[at].value;
}

int sh_alias_count(const sh_alias_table_t *t)
{
    return t == NULL ? 0 : t->count;
}

const char *sh_alias_name_at(const sh_alias_table_t *t, int i)
{
    return t != NULL && i >= 0 && i < t->count ? t->e[i].name : NULL;
}

const char *sh_alias_value_at(const sh_alias_table_t *t, int i)
{
    return t != NULL && i >= 0 && i < t->count ? t->e[i].value : NULL;
}

int sh_alias_seed(sh_alias_table_t *t)
{
    static const char *const seed[][2] = {
        {"l", "ls -l"}, {"ll", "ls -la"}, {"la", "ls -a"}, {"..", "cd .."}, {"...", "cd ../.."},
    };
    size_t i;

    for (i = 0; i < sizeof(seed) / sizeof(seed[0]); i++) {
        if (sh_alias_set(t, seed[i][0], seed[i][1]) != 0) {
            return -1;
        }
    }
    return 0;
}
