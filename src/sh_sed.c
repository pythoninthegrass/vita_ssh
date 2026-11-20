#include "sh_sed.h"

#include "sh_grep.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char op;                  /* 's', 'd', 'p' or 'q' */
    int global;               /* s///g */
    sh_grep_t *re;            /* s only */
    char repl[SH_SED_REPL_MAX];
    size_t repl_len;
} sed_cmd_t;

struct sh_sed {
    sed_cmd_t cmd[SH_SED_CMD_MAX];
    int n;
    char *space[2]; /* the pattern space and the substitution's output, swapped after a hit */
};

/* How much of the script a refusal quotes back. */
#define EXCERPT_MAX 24

/* In a stored replacement, this byte stands for & (a NUL cannot come from a script string). */
#define REPL_MATCH '\0'

sh_sed_t *sh_sed_new(void)
{
    return calloc(1, sizeof(sh_sed_t));
}

static void unwind(sh_sed_t *p, int keep)
{
    while (p->n > keep) {
        sh_grep_free(p->cmd[--p->n].re);
        p->cmd[p->n].re = NULL;
    }
}

void sh_sed_free(sh_sed_t *p)
{
    if (p == NULL) {
        return;
    }
    unwind(p, 0);
    free(p->space[0]);
    free(p->space[1]);
    free(p);
}

static void fail(char *why, size_t whylen, const char *fmt, ...)
{
    va_list ap;

    if (why == NULL || whylen == 0) {
        return;
    }
    va_start(ap, fmt);
    vsnprintf(why, whylen, fmt, ap);
    va_end(ap);
}

/* The script from p to the end of its line, shortened, for a refusal to quote. */
static const char *excerpt(const char *p)
{
    static char buf[EXCERPT_MAX + 4];
    size_t n = strcspn(p, "\n");

    if (n > EXCERPT_MAX) {
        memcpy(buf, p, EXCERPT_MAX);
        strcpy(buf + EXCERPT_MAX, "...");
    } else {
        memcpy(buf, p, n);
        buf[n] = '\0';
    }
    return buf;
}

static int is_blank(char c)
{
    return c == ' ' || c == '\t';
}

/* True at the end of a command: the end of the script, a separator, or a comment. */
static int ends_command(char c)
{
    return c == '\0' || c == ';' || c == '\n' || c == '#';
}

/* Parses s/RE/TEXT/[g] starting at the s; q is left after the last byte used. */
static int parse_subst(const char **q, sed_cmd_t *c, char *why, size_t whylen)
{
    const char *p = *q;
    char delim = p[1];
    char pat[SH_GREP_PATTERN_MAX + 1];
    size_t plen = 0;
    char errbuf[96];

    if (delim == '\0') {
        fail(why, whylen, "unterminated `s' command: %s", excerpt(*q));
        return -1;
    }
    if (delim == '\\' || delim == '\n') {
        fail(why, whylen, "invalid delimiter for `s': %s", excerpt(*q));
        return -1;
    }
    for (p += 2;; p++) {
        if (*p == '\0' || *p == '\n') {
            fail(why, whylen, "unterminated `s' command: %s", excerpt(*q));
            return -1;
        }
        if (*p == delim) {
            break;
        }
        if (*p == '\\') {
            if (p[1] == '\0') {
                fail(why, whylen, "unterminated `s' command: %s", excerpt(*q));
                return -1;
            }
            if (p[1] == delim) {
                p++; /* \delim is the delimiter itself, so a path needs no backslashes */
            } else {
                if (plen + 2 > SH_GREP_PATTERN_MAX) {
                    fail(why, whylen, "regular expression too long (limit %d bytes)",
                         SH_GREP_PATTERN_MAX);
                    return -1;
                }
                pat[plen++] = *p++;
            }
        }
        if (plen + 1 > SH_GREP_PATTERN_MAX) {
            fail(why, whylen, "regular expression too long (limit %d bytes)",
                 SH_GREP_PATTERN_MAX);
            return -1;
        }
        pat[plen++] = *p;
    }
    pat[plen] = '\0';
    if (plen == 0) {
        fail(why, whylen, "empty regular expression is not supported: %s", excerpt(*q));
        return -1;
    }
    c->re = sh_grep_compile(pat, 0);
    if (c->re == NULL) {
        sh_grep_compile_error(errbuf, sizeof(errbuf));
        fail(why, whylen, "regular expression %s: %s", pat,
             errbuf[0] != '\0' ? errbuf : "invalid");
        return -1;
    }

    for (p++;; p++) {
        char ch = *p;

        if (ch == '\0' || ch == '\n') {
            fail(why, whylen, "unterminated `s' command: %s", excerpt(*q));
            return -1;
        }
        if (ch == delim) {
            break;
        }
        if (c->repl_len + 1 >= SH_SED_REPL_MAX) {
            fail(why, whylen, "replacement too long (limit %d bytes)", SH_SED_REPL_MAX - 1);
            return -1;
        }
        if (ch == '&') {
            c->repl[c->repl_len++] = REPL_MATCH;
            continue;
        }
        if (ch == '\\') {
            ch = *++p;
            if (ch == '\0') {
                fail(why, whylen, "unterminated `s' command: %s", excerpt(*q));
                return -1;
            }
            if (ch >= '0' && ch <= '9') {
                fail(why, whylen, "back-references (\\%c) are not supported", ch);
                return -1;
            }
            if (ch != delim && ch != '&' && ch != '\\') {
                fail(why, whylen, "\\%c in a replacement is not supported", ch);
                return -1;
            }
        }
        c->repl[c->repl_len++] = ch;
    }

    for (p++; !ends_command(*p) && !is_blank(*p) && *p != '}'; p++) {
        if (*p != 'g') {
            fail(why, whylen, "flag '%c' of `s' is not supported: %s", *p, excerpt(*q));
            return -1;
        }
        c->global = 1;
    }
    *q = p;
    return 0;
}

int sh_sed_add(sh_sed_t *p, const char *script, char *why, size_t whylen)
{
    const char *q = script;
    int keep = p->n;

    if (why != NULL && whylen > 0) {
        why[0] = '\0';
    }
    for (;;) {
        char op;
        sed_cmd_t *c;

        while (is_blank(*q) || *q == ';' || *q == '\n') {
            q++;
        }
        if (*q == '\0') {
            return 0;
        }
        if (*q == '#') {
            q += strcspn(q, "\n");
            continue;
        }
        op = *q;
        if ((op >= '0' && op <= '9') || strchr("$/\\,!+~", op) != NULL) {
            fail(why, whylen, "line addresses are not supported: %s", excerpt(q));
            goto refused;
        }
        if (op == '{' || op == '}') {
            fail(why, whylen, "blocks are not supported: %s", excerpt(q));
            goto refused;
        }
        if (op == ':') {
            fail(why, whylen, "labels are not supported: %s", excerpt(q));
            goto refused;
        }
        if (op == 'b' || op == 't' || op == 'T') {
            fail(why, whylen, "branches are not supported: %s", excerpt(q));
            goto refused;
        }
        if (strchr("hHgGx", op) != NULL) {
            fail(why, whylen, "hold space commands are not supported: %s", excerpt(q));
            goto refused;
        }
        if (strchr("sdpq", op) == NULL) {
            if ((op >= 'a' && op <= 'z') || (op >= 'A' && op <= 'Z') || op == '=') {
                fail(why, whylen, "command '%c' is not supported: %s", op, excerpt(q));
            } else {
                fail(why, whylen, "unknown command: %s", excerpt(q));
            }
            goto refused;
        }
        if (p->n >= SH_SED_CMD_MAX) {
            fail(why, whylen, "too many commands (limit %d)", SH_SED_CMD_MAX);
            goto refused;
        }
        c = &p->cmd[p->n];
        memset(c, 0, sizeof(*c));
        c->op = op;
        p->n++;
        if (op == 's') {
            if (parse_subst(&q, c, why, whylen) != 0) {
                goto refused;
            }
        } else {
            q++;
        }
        while (is_blank(*q)) {
            q++;
        }
        if (!ends_command(*q)) {
            if (op == 'q' && *q >= '0' && *q <= '9') {
                fail(why, whylen, "an exit status after q is not supported: %s",
                     excerpt(q));
            } else if (*q == '}') {
                fail(why, whylen, "blocks are not supported: %s", excerpt(q));
            } else {
                fail(why, whylen, "extra characters after command: %s", excerpt(q));
            }
            goto refused;
        }
    }
refused:
    unwind(p, keep);
    return -1;
}

/* A growing copy into a buffer that may not pass SH_SED_SPACE_MAX. */
typedef struct {
    char *buf;
    size_t len;
    int over;
} sed_out_t;

static void out_put(sed_out_t *o, const char *s, size_t n)
{
    if (o->over || n > SH_SED_SPACE_MAX - o->len) {
        o->over = 1;
        return;
    }
    memcpy(o->buf + o->len, s, n);
    o->len += n;
}

/* Substitutes in[0..inlen) into out: 1 when something changed, 0 when not, -1 on error. */
static int substitute(const sed_cmd_t *c, const char *in, size_t inlen, sed_out_t *out)
{
    size_t pos = 0;
    long last_end = -1;
    int changed = 0;

    out->len = 0;
    out->over = 0;
    while (pos <= inlen) {
        size_t so, eo, i;
        int r = sh_grep_span(c->re, in, pos, &so, &eo);

        if (r < 0) {
            return -1;
        }
        if (r == 0) {
            break;
        }
        if (so == eo && (long)so == last_end) {
            /* An empty match right where the last one ended is skipped, as GNU sed does. */
            if (so >= inlen) {
                break;
            }
            out_put(out, in + pos, so + 1 - pos);
            pos = so + 1;
            continue;
        }
        out_put(out, in + pos, so - pos);
        for (i = 0; i < c->repl_len; i++) {
            size_t run = i;

            if (c->repl[i] == REPL_MATCH) {
                out_put(out, in + so, eo - so);
                continue;
            }
            while (run < c->repl_len && c->repl[run] != REPL_MATCH) {
                run++;
            }
            out_put(out, c->repl + i, run - i);
            i = run - 1;
        }
        changed = 1;
        last_end = (long)eo;
        if (eo == so) {
            if (so < inlen) {
                out_put(out, in + so, 1);
            }
            pos = so + 1;
        } else {
            pos = eo;
        }
        if (!c->global) {
            break;
        }
    }
    if (pos < inlen) {
        out_put(out, in + pos, inlen - pos);
    }
    if (out->over) {
        errno = ERANGE;
        return -1;
    }
    return changed;
}

int sh_sed_apply(sh_sed_t *p, const char *line, int quiet, sh_sed_emit_fn emit, void *ctx)
{
    size_t len = strlen(line);
    int cur = 0, i;

    if (len > SH_SED_SPACE_MAX) {
        errno = ERANGE;
        return -1;
    }
    if (p->space[0] == NULL) {
        p->space[0] = malloc(SH_SED_SPACE_MAX + 1);
        p->space[1] = malloc(SH_SED_SPACE_MAX + 1);
        if (p->space[0] == NULL || p->space[1] == NULL) {
            free(p->space[0]);
            free(p->space[1]);
            p->space[0] = p->space[1] = NULL;
            errno = ENOMEM;
            return -1;
        }
    }
    memcpy(p->space[cur], line, len + 1);

    for (i = 0; i < p->n; i++) {
        const sed_cmd_t *c = &p->cmd[i];

        switch (c->op) {
        case 's': {
            sed_out_t out;
            int r;

            out.buf = p->space[1 - cur];
            r = substitute(c, p->space[cur], len, &out);
            if (r < 0) {
                return -1;
            }
            if (r > 0) {
                out.buf[out.len] = '\0';
                cur = 1 - cur;
                len = out.len;
            }
            break;
        }
        case 'p':
            emit(ctx, p->space[cur], len);
            break;
        case 'd':
            return SH_SED_NEXT; /* the cycle ends here: no later command, no automatic print */
        case 'q':
            if (!quiet) {
                emit(ctx, p->space[cur], len);
            }
            return SH_SED_QUIT;
        default:
            break;
        }
    }
    if (!quiet) {
        emit(ctx, p->space[cur], len);
    }
    return SH_SED_NEXT;
}
