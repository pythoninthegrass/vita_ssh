/* Per-session environment variables; see include/sh_env.h. */
#include "sh_env.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

struct sh_env_table {
    int count;
    char names[SH_ENV_MAX][SH_ENV_NAME_MAX + 1];
    char values[SH_ENV_MAX][SH_ENV_VALUE_MAX + 1];
    char locked[SH_ENV_MAX];
};

sh_env_table_t *sh_env_new(void)
{
    return calloc(1, sizeof(struct sh_env_table));
}

void sh_env_free(sh_env_table_t *t)
{
    free(t);
}

static const char *const reserved_names[] = {"HOME", "PWD", "USER", "LOGNAME", "TMPDIR"};

int sh_env_name_ok(const char *name)
{
    size_t i;

    if (name == NULL || name[0] == '\0' || (name[0] >= '0' && name[0] <= '9')) {
        return 0;
    }
    for (i = 0; name[i] != '\0'; i++) {
        char c = name[i];

        if (i >= SH_ENV_NAME_MAX) {
            return 0;
        }
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) {
            return 0;
        }
    }
    return 1;
}

static int find(const sh_env_table_t *t, const char *name)
{
    int i;

    for (i = 0; i < t->count; i++) {
        if (strcmp(t->names[i], name) == 0) {
            return i;
        }
    }
    return -1;
}

size_t sh_env_strip_ctrl(char *dst, size_t size, const char *src)
{
    size_t n = 0;

    if (size == 0) {
        return 0;
    }
    for (; src != NULL && *src != '\0' && n < size - 1; src++) {
        unsigned char c = (unsigned char)*src;

        if (c >= 0x20 && c != 0x7F) {
            dst[n++] = (char)c;
        }
    }
    dst[n] = '\0';
    return n;
}

int sh_env_set(sh_env_table_t *t, const char *name, const char *value)
{
    char clean[SH_ENV_VALUE_MAX + 1];
    size_t n = 0;
    int i;

    if (t == NULL || value == NULL || !sh_env_name_ok(name)) {
        errno = EINVAL;
        return -1;
    }
    for (; *value != '\0'; value++) {
        unsigned char c = (unsigned char)*value;

        if (c < 0x20 || c == 0x7F) {
            continue;
        }
        if (n >= SH_ENV_VALUE_MAX) {
            errno = ENAMETOOLONG;
            return -1;
        }
        clean[n++] = (char)c;
    }
    clean[n] = '\0';
    i = find(t, name);
    if (i < 0) {
        if (t->count >= SH_ENV_MAX) {
            errno = ENOSPC;
            return -1;
        }
        i = t->count++;
        strcpy(t->names[i], name);
    }
    memcpy(t->values[i], clean, n + 1);
    return 0;
}

const char *sh_env_get(const sh_env_table_t *t, const char *name)
{
    int i;

    if (t == NULL || name == NULL) {
        return NULL;
    }
    i = find(t, name);
    return i < 0 ? NULL : t->values[i];
}

int sh_env_count(const sh_env_table_t *t)
{
    return t == NULL ? 0 : t->count;
}

const char *sh_env_name_at(const sh_env_table_t *t, int i)
{
    return t == NULL || i < 0 || i >= t->count ? NULL : t->names[i];
}

const char *sh_env_value_at(const sh_env_table_t *t, int i)
{
    return t == NULL || i < 0 || i >= t->count ? NULL : t->values[i];
}

int sh_env_unset(sh_env_table_t *t, const char *name)
{
    int i;

    if (t == NULL || name == NULL) {
        errno = EINVAL;
        return -1;
    }
    i = find(t, name);
    if (i < 0) {
        errno = ENOENT;
        return -1;
    }
    for (; i < t->count - 1; i++) {
        memcpy(t->names[i], t->names[i + 1], sizeof(t->names[i]));
        memcpy(t->values[i], t->values[i + 1], sizeof(t->values[i]));
        t->locked[i] = t->locked[i + 1];
    }
    t->count--;
    t->locked[t->count] = 0;
    return 0;
}

int sh_env_lock(sh_env_table_t *t, const char *name)
{
    int i = t == NULL || name == NULL ? -1 : find(t, name);

    if (i < 0) {
        errno = ENOENT;
        return -1;
    }
    t->locked[i] = 1;
    return 0;
}

int sh_env_is_readonly(const sh_env_table_t *t, const char *name)
{
    size_t i;
    int at;

    if (name == NULL) {
        return 0;
    }
    for (i = 0; i < sizeof(reserved_names) / sizeof(reserved_names[0]); i++) {
        if (strcmp(name, reserved_names[i]) == 0) {
            return 1;
        }
    }
    at = t == NULL ? -1 : find(t, name);
    return at >= 0 && t->locked[at];
}

sh_env_table_t *sh_env_snapshot(const sh_env_table_t *t)
{
    sh_env_table_t *copy = t == NULL ? NULL : sh_env_new();

    if (copy != NULL) {
        memcpy(copy, t, sizeof(*copy));
    }
    return copy;
}

void sh_env_restore(sh_env_table_t *t, sh_env_table_t *saved)
{
    if (t != NULL && saved != NULL) {
        memcpy(t, saved, sizeof(*t));
    }
    free(saved);
}
