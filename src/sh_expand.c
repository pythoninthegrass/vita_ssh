/* Variable expansion for one shell word; see include/sh_expand.h. */
#include "sh_expand.h"

#include <string.h>

#define NAME_BUF 64

typedef struct {
    char *out;
    size_t cap;
    size_t len;
    int nfields;
    int open;     /* a field is being built; its terminator is already reserved */
    int ws_ended; /* the last field was ended by IFS whitespace, so a delimiter right after it is part of it */
} fields_t;

static int is_name_start(char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}

static int is_name_char(char c)
{
    return is_name_start(c) || (c >= '0' && c <= '9');
}

/* Appends n bytes to the current field, opening one. Fails when the bytes and a terminator do not fit. */
static int add(fields_t *f, const char *src, size_t n)
{
    if (f->cap == 0 || f->len + n + 1 > f->cap) {
        return -1;
    }
    memcpy(f->out + f->len, src, n);
    f->len += n;
    f->open = 1;
    if (n > 0) {
        f->ws_ended = 0;
    }
    return 0;
}

/* Ends the current field, or writes an empty one when none is open. */
static int end_field(fields_t *f)
{
    if (f->len + 1 > f->cap) {
        return -1;
    }
    f->out[f->len++] = '\0';
    f->nfields++;
    f->open = 0;
    return 0;
}

static int is_ifs_space(char c)
{
    return c == ' ' || c == '\t' || c == '\n';
}

/* Splits value at the characters of ifs into the fields, as POSIX describes for an unquoted expansion. */
static int add_split(fields_t *f, const char *value, const char *ifs)
{
    const char *v;

    if (ifs[0] == '\0') {
        return value[0] == '\0' ? 0 : add(f, value, strlen(value));
    }
    for (v = value; *v != '\0'; v++) {
        if (strchr(ifs, *v) == NULL) {
            if (add(f, v, 1) != 0) {
                return -1;
            }
        } else if (is_ifs_space(*v)) {
            if (f->open) {
                if (end_field(f) != 0) {
                    return -1;
                }
                f->ws_ended = 1;
            }
        } else if (f->open) {
            if (end_field(f) != 0) {
                return -1;
            }
            f->ws_ended = 0;
        } else if (f->ws_ended) {
            f->ws_ended = 0;
        } else if (end_field(f) != 0) {
            return -1;
        }
    }
    return 0;
}

/* Records the construct as written: prefix, then up to n bytes of the word, cut to fit. */
static void note(sh_expand_info_t *info, const char *prefix, const char *text, size_t n)
{
    size_t room = sizeof(info->what) - 1;
    size_t p = strlen(prefix);

    if (p > room) {
        p = room;
    }
    memcpy(info->what, prefix, p);
    if (n > room - p) {
        n = room - p;
    }
    memcpy(info->what + p, text, n);
    info->what[p + n] = '\0';
}

static int is_special_param(char c)
{
    return strchr("?#$!@*-", c) != NULL || (c >= '0' && c <= '9');
}

/* A reference starts at the `$` at p[0]. On success sets *used to the bytes it covers and *name to the
 * variable (NULL when the name is too long to ever have been set); otherwise returns the failure and
 * fills info->what. Returns SH_EXPAND_OK with *used == 0 for a `$` that is plain text. */
static sh_expand_status_t parse_ref(const char *p, char *buf, const char **name, size_t *used, sh_expand_info_t *info)
{
    size_t n = 0;

    *name = NULL;
    *used = 0;
    if (p[1] == '(') {
        if (p[2] == '(') {
            note(info, "$((", "", 0);
        } else {
            note(info, "$(", "", 0);
        }
        return SH_EXPAND_UNSUPPORTED;
    }
    if (p[1] == '{') {
        const char *q = p + 2;
        size_t named = 0;

        while (is_name_char(q[named])) {
            named++;
        }
        if (named > 0 && q[0] >= '0' && q[0] <= '9') {
            note(info, "${", q, named);
            return SH_EXPAND_UNSUPPORTED;
        }
        if (named == 0 || q[named] != '}') {
            char c = q[named];

            if (named > 0 && c != '\0' && strchr(":-=+?#%/^,@!*", c) != NULL) {
                note(info, "${", q, named + 1);
                return SH_EXPAND_UNSUPPORTED;
            }
            note(info, "${", q, c == '\0' ? named : named + 1);
            return SH_EXPAND_MALFORMED;
        }
        *used = 3 + named;
        if (named < NAME_BUF) {
            memcpy(buf, q, named);
            buf[named] = '\0';
            *name = buf;
        }
        return SH_EXPAND_OK;
    }
    if (is_name_start(p[1])) {
        while (is_name_char(p[1 + n])) {
            n++;
        }
        *used = 1 + n;
        if (n < NAME_BUF) {
            memcpy(buf, p + 1, n);
            buf[n] = '\0';
            *name = buf;
        }
        return SH_EXPAND_OK;
    }
    if (p[1] != '\0' && is_special_param(p[1])) {
        note(info, "$", p + 1, 1);
        return SH_EXPAND_UNSUPPORTED;
    }
    return SH_EXPAND_OK;
}

sh_expand_status_t sh_expand_word(const char *word, sh_expand_lookup_fn lookup, void *ctx, const char *ifs, char *out,
                                  size_t cap, sh_expand_info_t *info)
{
    fields_t f = {out, cap, 0, 0, 0, 0};
    const char *p;

    memset(info, 0, sizeof(*info));
    if (ifs == NULL) {
        ifs = " \t\n";
    }
    for (p = word; *p != '\0';) {
        char buf[NAME_BUF];
        const char *name;
        const char *value;
        size_t used;
        int split;
        sh_expand_status_t st;

        if (*p == SH_EXPAND_LITERAL_DOLLAR) {
            if (add(&f, "$", 1) != 0) {
                return SH_EXPAND_TOOLONG;
            }
            p++;
            continue;
        }
        if (*p != '$' && *p != SH_EXPAND_QUOTED_DOLLAR) {
            if (add(&f, p, 1) != 0) {
                return SH_EXPAND_TOOLONG;
            }
            p++;
            continue;
        }
        split = *p == '$';
        st = parse_ref(p, buf, &name, &used, info);
        if (st != SH_EXPAND_OK) {
            return st;
        }
        if (used == 0) {
            if (add(&f, "$", 1) != 0) {
                return SH_EXPAND_TOOLONG;
            }
            p++;
            continue;
        }
        info->refs++;
        value = name != NULL ? lookup(ctx, name) : NULL;
        if (split) {
            if (value != NULL && add_split(&f, value, ifs) != 0) {
                return SH_EXPAND_TOOLONG;
            }
        } else if (add(&f, value != NULL ? value : "", value != NULL ? strlen(value) : 0) != 0) {
            return SH_EXPAND_TOOLONG;
        }
        p += used;
    }
    if (f.open && end_field(&f) != 0) {
        return SH_EXPAND_TOOLONG;
    }
    if (f.cap == 0) {
        return SH_EXPAND_TOOLONG;
    }
    if (f.nfields == 0) {
        out[0] = '\0';
    }
    info->nfields = f.nfields;
    return SH_EXPAND_OK;
}
