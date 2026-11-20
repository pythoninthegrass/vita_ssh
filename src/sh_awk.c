#include "sh_awk.h"

#include "sh_grep.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { OP_FIELD, OP_LAST, OP_NF, OP_NR, OP_NUM, OP_STR };
enum { REL_EQ, REL_NE, REL_LT, REL_LE, REL_GT, REL_GE };

typedef struct {
    int kind;
    long n;             /* OP_FIELD: the field number, 0 for the whole line */
    double num;         /* OP_NUM */
    const char *text;   /* OP_STR and OP_NUM: the string form, in the program's pool */
    size_t len;
} operand_t;

typedef struct {
    int first;
    int count;
} stmt_t;

/* Print arguments across every statement. */
#define ARG_TOTAL 64

/* String literals across the program, NUL included. */
#define POOL_MAX 1024

/* Fields can be numbered past what any line has, but not past what the grammar can say. */
#define FIELD_NUM_DIGITS 6

/* How much of the program a refusal quotes back. */
#define EXCERPT_MAX 24

struct sh_awk {
    sh_grep_t *re;                 /* /RE/ pattern, or NULL */
    int has_cmp;                   /* OPERAND RELOP OPERAND pattern */
    operand_t lhs, rhs;
    int relop;
    int nstmt;
    stmt_t stmt[SH_AWK_STMT_MAX];
    int nargs;
    operand_t args[ARG_TOTAL];
    int needs_fields;              /* the program reads a field, NF or $NF */
    int sep;                       /* the one separator byte, or -1 for runs of blanks */
    long nr;
    char pool[POOL_MAX];
    size_t pool_used;
    size_t fstart[SH_AWK_FIELD_MAX], fend[SH_AWK_FIELD_MAX];
    int nf;
    char out[SH_AWK_OUT_MAX + 1];
};

sh_awk_t *sh_awk_new(void)
{
    return calloc(1, sizeof(sh_awk_t));
}

void sh_awk_free(sh_awk_t *p)
{
    if (p == NULL) {
        return;
    }
    sh_grep_free(p->re);
    free(p);
}

typedef struct {
    sh_awk_t *a;
    const char *p;
    char *why;
    size_t whylen;
} parser_t;

static void fail(parser_t *ps, const char *fmt, ...)
{
    va_list ap;

    if (ps->why == NULL || ps->whylen == 0) {
        return;
    }
    va_start(ap, fmt);
    vsnprintf(ps->why, ps->whylen, fmt, ap);
    va_end(ap);
}

/* The program from p to the end of its line, shortened, for a refusal to quote. */
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
    return c == ' ' || c == '\t' || c == '\r';
}

static int is_digit(char c)
{
    return c >= '0' && c <= '9';
}

static int is_word_start(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

static int is_word_char(char c)
{
    return is_word_start(c) || is_digit(c);
}

static void skip_blank(parser_t *ps)
{
    while (is_blank(*ps->p)) {
        ps->p++;
    }
}

static void skip_ws(parser_t *ps)
{
    while (is_blank(*ps->p) || *ps->p == '\n') {
        ps->p++;
    }
}

/* True at the end of a statement or of the action. */
static int ends_statement(char c)
{
    return c == '\0' || c == ';' || c == '}' || c == '\n';
}

/* ------------------------------------------------------------------ */
/* Refusals                                                             */
/* ------------------------------------------------------------------ */

static int refuse(parser_t *ps, const char *what)
{
    fail(ps, "unsupported: %s", what);
    return -1;
}

static int syntax(parser_t *ps)
{
    if (*ps->p == '\0') {
        fail(ps, "unexpected end of program");
    } else {
        fail(ps, "syntax error near '%s'", excerpt(ps->p));
    }
    return -1;
}

/* An assignment operator at p: = (but not ==), op=, ++ or --. */
static int at_assignment(const char *p)
{
    if (*p == '=') {
        return p[1] != '=';
    }
    if (strchr("+-*/%^", *p) != NULL && *p != '\0') {
        return p[1] == '=' || ((*p == '+' || *p == '-') && p[1] == *p);
    }
    return 0;
}

static const char *const keywords[] = {
    "BEGIN", "END", "if", "else", "while", "for", "do", "function", "func", "getline",
    "printf", "next", "nextfile", "exit", "delete", "return", "break", "continue", "in",
};

/* Reads a word at ps->p into w (cut to wlen - 1); returns its true length, 0 if none. */
static size_t read_word(parser_t *ps, char *w, size_t wlen)
{
    size_t n = 0;

    while (is_word_char(ps->p[n])) {
        n++;
    }
    if (wlen > 0) {
        size_t c = n < wlen - 1 ? n : wlen - 1;

        memcpy(w, ps->p, c);
        w[c] = '\0';
    }
    ps->p += n;
    return n;
}

/* A word the subset has no use for: a keyword, a call, an assignment or a variable. */
static int refuse_word(parser_t *ps, const char *w)
{
    size_t i;
    const char *q;

    for (i = 0; i < sizeof(keywords) / sizeof(keywords[0]); i++) {
        if (strcmp(w, keywords[i]) == 0) {
            return refuse(ps, strcmp(w, "func") == 0 ? "function" : w);
        }
    }
    for (q = ps->p; is_blank(*q); q++) {
    }
    if (*q == '(' && ps->p == q) {
        fail(ps, "unsupported: function call %s", w);
        return -1;
    }
    if (at_assignment(q)) {
        return refuse(ps, "assignment");
    }
    fail(ps, "unsupported: variable %s", w);
    return -1;
}

/* ------------------------------------------------------------------ */
/* Operands                                                             */
/* ------------------------------------------------------------------ */

/* The string form of a number: integral values print as integers, as awk does. */
static size_t render_num(double d, char *dst, size_t cap)
{
    int n;

    if (d > -1e15 && d < 1e15 && d == (double)(long long)d) {
        n = snprintf(dst, cap, "%lld", (long long)d);
    } else {
        n = snprintf(dst, cap, "%.6g", d);
    }
    return (size_t)n < cap ? (size_t)n : cap - 1;
}

/* Copies text into the program's pool, NUL-terminated; NULL when the pool is full. */
static const char *pool_add(parser_t *ps, const char *text, size_t len)
{
    sh_awk_t *a = ps->a;
    char *dst;

    if (len + 1 > POOL_MAX - a->pool_used) {
        fail(ps, "string literals too long (limit %d bytes in all)", POOL_MAX);
        return NULL;
    }
    dst = a->pool + a->pool_used;
    memcpy(dst, text, len);
    dst[len] = '\0';
    a->pool_used += len + 1;
    return dst;
}

static int parse_string(parser_t *ps, operand_t *o)
{
    char buf[SH_AWK_STR_MAX + 1];
    size_t n = 0;

    for (ps->p++;; ps->p++) {
        char c = *ps->p;

        if (c == '\0' || c == '\n') {
            fail(ps, "unterminated string");
            return -1;
        }
        if (c == '"') {
            ps->p++;
            break;
        }
        if (c == '\\') {
            ps->p++;
            switch (*ps->p) {
            case 'n': c = '\n'; break;
            case 't': c = '\t'; break;
            case '\\': c = '\\'; break;
            case '"': c = '"'; break;
            case '/': c = '/'; break;
            default:
                fail(ps, "bad escape \\%c in a string", *ps->p == '\0' ? ' ' : *ps->p);
                return -1;
            }
        }
        if (n >= SH_AWK_STR_MAX) {
            fail(ps, "string too long (limit %d bytes)", SH_AWK_STR_MAX);
            return -1;
        }
        buf[n++] = c;
    }
    o->kind = OP_STR;
    o->len = n;
    o->text = pool_add(ps, buf, n);
    return o->text == NULL ? -1 : 0;
}

static int parse_number(parser_t *ps, operand_t *o)
{
    const char *start = ps->p;
    char buf[64], text[40];
    size_t n;

    if (*ps->p == '-') {
        ps->p++;
    }
    while (is_digit(*ps->p)) {
        ps->p++;
    }
    if (*ps->p == '.') {
        ps->p++;
        while (is_digit(*ps->p)) {
            ps->p++;
        }
    }
    if ((*ps->p == 'e' || *ps->p == 'E') &&
        (is_digit(ps->p[1]) || ((ps->p[1] == '+' || ps->p[1] == '-') && is_digit(ps->p[2])))) {
        ps->p += 2;
        while (is_digit(*ps->p)) {
            ps->p++;
        }
    }
    n = (size_t)(ps->p - start);
    if (n >= sizeof(buf)) {
        fail(ps, "number too long");
        return -1;
    }
    memcpy(buf, start, n);
    buf[n] = '\0';
    o->kind = OP_NUM;
    o->num = strtod(buf, NULL);
    o->len = render_num(o->num, text, sizeof(text));
    o->text = pool_add(ps, text, o->len);
    return o->text == NULL ? -1 : 0;
}

static int parse_field(parser_t *ps, operand_t *o)
{
    char w[16];
    long n = 0;
    int digits = 0;

    ps->p++; /* the $ */
    if (is_digit(*ps->p)) {
        while (is_digit(*ps->p)) {
            if (++digits > FIELD_NUM_DIGITS) {
                fail(ps, "field number too large");
                return -1;
            }
            n = n * 10 + (*ps->p++ - '0');
        }
        o->kind = OP_FIELD;
        o->n = n;
    } else if (is_word_start(*ps->p)) {
        if (read_word(ps, w, sizeof(w)) == 2 && strcmp(w, "NF") == 0) {
            o->kind = OP_LAST;
        } else {
            return refuse_word(ps, w);
        }
    } else if (*ps->p == '(') {
        return refuse(ps, "$(expression)");
    } else {
        fail(ps, "expected a field number after $");
        return -1;
    }
    ps->a->needs_fields = 1;
    return 0;
}

static int parse_operand(parser_t *ps, operand_t *o)
{
    char c;
    char w[32];

    skip_blank(ps);
    c = *ps->p;
    memset(o, 0, sizeof(*o));
    if (c == '$') {
        return parse_field(ps, o);
    }
    if (c == '"') {
        return parse_string(ps, o);
    }
    if (is_digit(c) || (c == '.' && is_digit(ps->p[1])) ||
        (c == '-' && (is_digit(ps->p[1]) || (ps->p[1] == '.' && is_digit(ps->p[2]))))) {
        return parse_number(ps, o);
    }
    if (is_word_start(c)) {
        size_t n = read_word(ps, w, sizeof(w));

        if (n == 2 && strcmp(w, "NF") == 0) {
            o->kind = OP_NF;
            ps->a->needs_fields = 1;
            return 0;
        }
        if (n == 2 && strcmp(w, "NR") == 0) {
            o->kind = OP_NR;
            return 0;
        }
        return refuse_word(ps, w);
    }
    if (c == '-' || c == '+') {
        return refuse(ps, "arithmetic");
    }
    if (c == '(') {
        return refuse(ps, "parentheses");
    }
    if (c == '\0' || c == '}' || c == ';' || c == '\n') {
        fail(ps, "expected an operand");
        return -1;
    }
    return syntax(ps);
}

/* ------------------------------------------------------------------ */
/* Regular expressions                                                  */
/* ------------------------------------------------------------------ */

/* Copies a bracket expression starting at the [ and returns how many bytes it took, 0 if open. */
static size_t bracket_len(const char *s)
{
    size_t i = 1;

    if (s[i] == '^') {
        i++;
    }
    if (s[i] == ']') {
        i++;
    }
    while (s[i] != '\0' && s[i] != ']') {
        if (s[i] == '[' && (s[i + 1] == ':' || s[i + 1] == '.' || s[i + 1] == '=')) {
            char kind = s[i + 1];
            const char *close = strstr(s + i + 2, (char[]){kind, ']', '\0'});

            if (close == NULL) {
                return 0;
            }
            i = (size_t)(close - s) + 2;
        } else {
            i++;
        }
    }
    return s[i] == ']' ? i + 1 : 0;
}

/* awk's regexes are POSIX extended and the grep engine's are basic, so + ? | ( ) { } are written
 * the other way round for it. Out holds at most 2 * strlen(re) + 1 bytes. */
static int ere_to_bre(parser_t *ps, const char *re, char *out)
{
    size_t o = 0;
    size_t i = 0;
    int at_start = 1; /* nothing but anchors-in-waiting before this: start, after ( or | */

    while (re[i] != '\0') {
        char c = re[i];

        if (c == '[') {
            size_t n = bracket_len(re + i);

            if (n == 0) {
                fail(ps, "unmatched [ in a regular expression");
                return -1;
            }
            memcpy(out + o, re + i, n);
            o += n;
            i += n;
            at_start = 0;
            continue;
        }
        if (c == '\\') {
            char e = re[i + 1];

            if (e == '\0') {
                fail(ps, "trailing backslash in a regular expression");
                return -1;
            }
            if ((e >= 'a' && e <= 'z') || (e >= 'A' && e <= 'Z') || is_digit(e)) {
                fail(ps, "unsupported: escape \\%c in a regular expression", e);
                return -1;
            }
            if (strchr(".*[]^$\\", e) != NULL) {
                out[o++] = '\\';
            }
            out[o++] = e;
            i += 2;
            at_start = 0;
            continue;
        }
        if (strchr("+?|(){}", c) != NULL) {
            out[o++] = '\\';
            out[o++] = c;
            at_start = (c == '(' || c == '|');
            i++;
            continue;
        }
        if (c == '^' && !at_start) {
            fail(ps, "unsupported: ^ inside a regular expression");
            return -1;
        }
        if (c == '$' && re[i + 1] != '\0' && re[i + 1] != ')' && re[i + 1] != '|') {
            fail(ps, "unsupported: $ inside a regular expression");
            return -1;
        }
        out[o++] = c;
        at_start = (c == '^') ? at_start : 0;
        i++;
    }
    out[o] = '\0';
    return 0;
}

static int parse_regex(parser_t *ps)
{
    char raw[SH_GREP_PATTERN_MAX + 1];
    char bre[2 * SH_GREP_PATTERN_MAX + 1];
    char why[96];
    size_t n = 0;

    for (ps->p++;; ps->p++) {
        char c = *ps->p;

        if (c == '\0' || c == '\n') {
            fail(ps, "unterminated regular expression");
            return -1;
        }
        if (c == '/') {
            ps->p++;
            break;
        }
        if (c == '\\' && ps->p[1] == '/') {
            c = '/';
            ps->p++;
        } else if (c == '\\' && ps->p[1] != '\0' && ps->p[1] != '\n') {
            if (n + 2 > SH_GREP_PATTERN_MAX) {
                goto too_long;
            }
            raw[n++] = c;
            c = *++ps->p;
        }
        if (n + 1 > SH_GREP_PATTERN_MAX) {
            goto too_long;
        }
        raw[n++] = c;
    }
    raw[n] = '\0';
    if (ere_to_bre(ps, raw, bre) != 0) {
        return -1;
    }
    ps->a->re = sh_grep_compile(bre, 0);
    if (ps->a->re == NULL) {
        fail(ps, "%s", sh_grep_compile_error(why, sizeof(why)));
        return -1;
    }
    return 0;

too_long:
    fail(ps, "regular expression too long (limit %d bytes)", SH_GREP_PATTERN_MAX);
    return -1;
}

/* ------------------------------------------------------------------ */
/* The program                                                          */
/* ------------------------------------------------------------------ */

/* At an operator that follows a complete pattern or print argument but is not part of the
 * subset: names it. Returns 0 when the byte at p is not such an operator. */
static int refuse_operator(parser_t *ps, int in_print)
{
    const char *p = ps->p;
    char c = *p;

    if (c == '~') {
        return refuse(ps, "~");
    }
    if (c == '!') {
        return refuse(ps, p[1] == '~' ? "!~" : p[1] == '=' ? "comparison in print" : "!");
    }
    if (c == '&' && p[1] == '&') {
        return refuse(ps, "&&");
    }
    if (c == '|' && p[1] == '|') {
        return refuse(ps, "||");
    }
    if (c == '?' || c == ':') {
        return refuse(ps, "?:");
    }
    if (c == ',') {
        return refuse(ps, "range pattern");
    }
    if (in_print) {
        if (strchr("+-*/%^", c) != NULL) {
            return refuse(ps, "arithmetic");
        }
        if (c == '>' && p[1] != '=') {
            return refuse(ps, "output redirection");
        }
        if (c == '|') {
            return refuse(ps, "output redirection");
        }
        if (c == '=' && p[1] != '=') {
            return refuse(ps, "assignment");
        }
        if (c == '=' || c == '<' || c == '>') {
            return refuse(ps, "comparison in print");
        }
        if (c == '$' || c == '"' || c == '(' || is_word_char(c) || c == '.') {
            return refuse(ps, "string concatenation (separate arguments with commas)");
        }
    }
    return 0;
}

static int parse_print(parser_t *ps)
{
    sh_awk_t *a = ps->a;
    stmt_t *st;

    if (a->nstmt >= SH_AWK_STMT_MAX) {
        fail(ps, "too many statements (limit %d)", SH_AWK_STMT_MAX);
        return -1;
    }
    st = &a->stmt[a->nstmt++];
    st->first = a->nargs;
    st->count = 0;
    skip_blank(ps);
    if (!ends_statement(*ps->p)) {
        for (;;) {
            if (st->count >= SH_AWK_PRINT_ARG_MAX || a->nargs >= ARG_TOTAL) {
                fail(ps, "too many print arguments (limit %d)", SH_AWK_PRINT_ARG_MAX);
                return -1;
            }
            if (parse_operand(ps, &a->args[a->nargs]) != 0) {
                return -1;
            }
            a->nargs++;
            st->count++;
            skip_blank(ps);
            if (*ps->p == ',') {
                ps->p++;
                continue;
            }
            if (ends_statement(*ps->p)) {
                break;
            }
            if (refuse_operator(ps, 1) != 0) {
                return -1;
            }
            return syntax(ps);
        }
    } else {
        /* A bare print is print $0. */
        if (a->nargs >= ARG_TOTAL) {
            fail(ps, "too many print arguments (limit %d)", SH_AWK_PRINT_ARG_MAX);
            return -1;
        }
        memset(&a->args[a->nargs], 0, sizeof(operand_t));
        a->args[a->nargs].kind = OP_FIELD;
        a->nargs++;
        st->count = 1;
    }
    return 0;
}

static int parse_action(parser_t *ps)
{
    char w[32];

    ps->p++; /* the { */
    for (;;) {
        size_t n;

        while (is_blank(*ps->p) || *ps->p == '\n' || *ps->p == ';') {
            ps->p++;
        }
        if (*ps->p == '}') {
            ps->p++;
            return 0;
        }
        if (*ps->p == '\0') {
            fail(ps, "missing }");
            return -1;
        }
        if (*ps->p == '$') {
            operand_t o;

            if (parse_operand(ps, &o) != 0) {
                return -1;
            }
            skip_blank(ps);
            if (at_assignment(ps->p)) {
                return refuse(ps, "assignment");
            }
            return syntax(ps);
        }
        if (!is_word_start(*ps->p)) {
            return syntax(ps);
        }
        n = read_word(ps, w, sizeof(w));
        if (n == 5 && strcmp(w, "print") == 0) {
            if (parse_print(ps) != 0) {
                return -1;
            }
            continue;
        }
        if ((n == 2 && strcmp(w, "NF") == 0) || (n == 2 && strcmp(w, "NR") == 0)) {
            skip_blank(ps);
            if (at_assignment(ps->p)) {
                return refuse(ps, "assignment");
            }
            fail(ps, "expected print, got %s", w);
            return -1;
        }
        return refuse_word(ps, w);
    }
}

static int parse_relop(parser_t *ps)
{
    const char *p = ps->p;
    int op, n = 2;

    if (p[0] == '=' && p[1] == '=') {
        op = REL_EQ;
    } else if (p[0] == '!' && p[1] == '=') {
        op = REL_NE;
    } else if (p[0] == '<' && p[1] == '=') {
        op = REL_LE;
    } else if (p[0] == '>' && p[1] == '=') {
        op = REL_GE;
    } else if (p[0] == '<') {
        op = REL_LT;
        n = 1;
    } else if (p[0] == '>') {
        op = REL_GT;
        n = 1;
    } else if (p[0] == '=') {
        return refuse(ps, "assignment");
    } else if (refuse_operator(ps, 0) != 0) {
        return -1;
    } else if (*p == '\0' || *p == '{') {
        fail(ps, "pattern must be /regex/ or a comparison");
        return -1;
    } else {
        return syntax(ps);
    }
    ps->p += n;
    ps->a->relop = op;
    return 0;
}

/* Parses the pattern, leaving ps->p after it. */
static int parse_pattern(parser_t *ps)
{
    sh_awk_t *a = ps->a;
    char w[32];
    const char *start = ps->p;

    if (*ps->p == '/') {
        return parse_regex(ps);
    }
    if (*ps->p == '!') {
        return refuse(ps, "!");
    }
    if (is_word_start(*ps->p)) {
        size_t n = read_word(ps, w, sizeof(w));

        if (n == 5 && strcmp(w, "print") == 0) {
            fail(ps, "print must be inside { }");
            return -1;
        }
        if (!((n == 2 && strcmp(w, "NF") == 0) || (n == 2 && strcmp(w, "NR") == 0))) {
            return refuse_word(ps, w);
        }
        ps->p = start;
    }
    if (parse_operand(ps, &a->lhs) != 0) {
        return -1;
    }
    skip_blank(ps);
    if (parse_relop(ps) != 0) {
        return -1;
    }
    if (parse_operand(ps, &a->rhs) != 0) {
        return -1;
    }
    a->has_cmp = 1;
    return 0;
}

static void reset(sh_awk_t *a)
{
    sh_grep_free(a->re);
    memset(a, 0, sizeof(*a));
}

static int set_separator(parser_t *ps, const char *fs)
{
    sh_awk_t *a = ps->a;

    a->sep = -1;
    if (fs == NULL) {
        return 0;
    }
    switch (strlen(fs)) {
    case 0:
        return refuse(ps, "empty -F");
    case 1:
        if (fs[0] == 't') {
            return refuse(ps, "-Ft (write -F '\\t' for a tab)");
        }
        a->sep = fs[0] == ' ' ? -1 : (unsigned char)fs[0];
        return 0;
    case 2:
        if (fs[0] == '\\' && fs[1] == 't') {
            a->sep = '\t';
            return 0;
        }
        if (fs[0] == '\\' && fs[1] == '\\') {
            a->sep = '\\';
            return 0;
        }
        /* fall through */
    default:
        return refuse(ps, "-F with more than one character");
    }
}

int sh_awk_compile(sh_awk_t *a, const char *program, const char *fs, char *why, size_t whylen)
{
    parser_t ps;

    ps.a = a;
    ps.p = program;
    ps.why = why;
    ps.whylen = whylen;
    reset(a);
    if (set_separator(&ps, fs) != 0) {
        goto bad;
    }
    skip_ws(&ps);
    if (*ps.p == '\0') {
        fail(&ps, "empty program");
        goto bad;
    }
    if (*ps.p != '{') {
        if (parse_pattern(&ps) != 0) {
            goto bad;
        }
        skip_blank(&ps);
        if (*ps.p != '{') {
            if (!ends_statement(*ps.p) || *ps.p == '}') {
                if (refuse_operator(&ps, 0) != 0 || syntax(&ps) != 0) {
                    goto bad;
                }
            }
            /* No action: print the line. */
            skip_ws(&ps);
            while (*ps.p == ';') {
                ps.p++;
                skip_ws(&ps);
            }
            if (*ps.p != '\0') {
                refuse(&ps, "multiple rules");
                goto bad;
            }
            a->nstmt = 1;
            a->stmt[0].first = 0;
            a->stmt[0].count = 1;
            a->args[0].kind = OP_FIELD;
            a->nargs = 1;
            return 0;
        }
    }
    if (parse_action(&ps) != 0) {
        goto bad;
    }
    skip_ws(&ps);
    while (*ps.p == ';') {
        ps.p++;
        skip_ws(&ps);
    }
    if (*ps.p == '}') {
        syntax(&ps);
        goto bad;
    }
    if (*ps.p != '\0') {
        refuse(&ps, "multiple rules");
        goto bad;
    }
    return 0;

bad:
    {
        char keep[160];

        /* reset() clears the program, never the reason. */
        snprintf(keep, sizeof(keep), "%s", why != NULL ? why : "");
        reset(a);
        if (why != NULL && whylen > 0) {
            snprintf(why, whylen, "%s", keep);
        }
    }
    return -1;
}

/* ------------------------------------------------------------------ */
/* Running                                                              */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *s;
    size_t len;
    int isnum;       /* compares as a number */
    double num;
    char tmp[40];    /* backing store for a rendered number */
} value_t;

static int blank_byte(char c)
{
    return c == ' ' || c == '\t' || c == '\n';
}

/* A field "looks numeric" the way awk decides: optional blanks around a decimal number. */
static int looks_numeric(const char *s, size_t len, double *out)
{
    char buf[128];
    size_t i = 0, end = len;
    size_t j;
    int digits = 0;

    while (i < end && blank_byte(s[i])) {
        i++;
    }
    while (end > i && blank_byte(s[end - 1])) {
        end--;
    }
    if (end == i || end - i >= sizeof(buf)) {
        return 0;
    }
    j = i;
    if (s[j] == '+' || s[j] == '-') {
        j++;
    }
    while (j < end && is_digit(s[j])) {
        j++;
        digits++;
    }
    if (j < end && s[j] == '.') {
        j++;
        while (j < end && is_digit(s[j])) {
            j++;
            digits++;
        }
    }
    if (digits == 0) {
        return 0;
    }
    if (j < end && (s[j] == 'e' || s[j] == 'E')) {
        size_t k = j + 1;
        int exp_digits = 0;

        if (k < end && (s[k] == '+' || s[k] == '-')) {
            k++;
        }
        while (k < end && is_digit(s[k])) {
            k++;
            exp_digits++;
        }
        if (exp_digits == 0) {
            return 0;
        }
        j = k;
    }
    if (j != end) {
        return 0;
    }
    memcpy(buf, s + i, end - i);
    buf[end - i] = '\0';
    *out = strtod(buf, NULL);
    return 1;
}

static void field_value(sh_awk_t *a, const char *line, long n, value_t *v)
{
    if (n == 0) {
        v->s = line;
        v->len = strlen(line);
    } else if (n <= a->nf) {
        v->s = line + a->fstart[n - 1];
        v->len = a->fend[n - 1] - a->fstart[n - 1];
    } else {
        /* A field the line does not have is "", which is not a number (pinned against awk). */
        v->s = "";
        v->len = 0;
        return;
    }
    v->isnum = looks_numeric(v->s, v->len, &v->num);
}

static void get_value(sh_awk_t *a, const char *line, const operand_t *o, value_t *v)
{
    memset(v, 0, sizeof(*v));
    switch (o->kind) {
    case OP_FIELD:
        field_value(a, line, o->n, v);
        break;
    case OP_LAST:
        field_value(a, line, a->nf, v);
        break;
    case OP_NF:
    case OP_NR:
        v->num = o->kind == OP_NF ? (double)a->nf : (double)a->nr;
        v->isnum = 1;
        v->len = render_num(v->num, v->tmp, sizeof(v->tmp));
        v->s = v->tmp;
        break;
    case OP_NUM:
        v->num = o->num;
        v->isnum = 1;
        v->s = o->text;
        v->len = o->len;
        break;
    default:
        v->s = o->text;
        v->len = o->len;
        break;
    }
}

static int compare_values(const value_t *l, const value_t *r)
{
    if (l->isnum && r->isnum) {
        return l->num < r->num ? -1 : l->num > r->num ? 1 : 0;
    } else {
        size_t n = l->len < r->len ? l->len : r->len;
        int c = memcmp(l->s, r->s, n);

        if (c != 0) {
            return c < 0 ? -1 : 1;
        }
        return l->len < r->len ? -1 : l->len > r->len ? 1 : 0;
    }
}

static int split_fields(sh_awk_t *a, const char *line)
{
    size_t i = 0, len = strlen(line);

    a->nf = 0;
    if (a->sep < 0) {
        for (;;) {
            size_t start;

            while (i < len && (line[i] == ' ' || line[i] == '\t')) {
                i++;
            }
            if (i >= len) {
                return 0;
            }
            start = i;
            while (i < len && line[i] != ' ' && line[i] != '\t') {
                i++;
            }
            if (a->nf >= SH_AWK_FIELD_MAX) {
                errno = ERANGE;
                return -1;
            }
            a->fstart[a->nf] = start;
            a->fend[a->nf++] = i;
        }
    }
    if (len == 0) {
        return 0;
    }
    for (;;) {
        size_t start = i;

        while (i < len && (unsigned char)line[i] != a->sep) {
            i++;
        }
        if (a->nf >= SH_AWK_FIELD_MAX) {
            errno = ERANGE;
            return -1;
        }
        a->fstart[a->nf] = start;
        a->fend[a->nf++] = i;
        if (i >= len) {
            return 0;
        }
        i++;
    }
}

/* 1 when the line passes the pattern, 0 when not, -1 with errno from the regex engine. */
static int pattern_matches(sh_awk_t *a, const char *line)
{
    value_t l, r;
    int c;

    if (a->re != NULL) {
        return sh_grep_match(a->re, line, 0);
    }
    if (!a->has_cmp) {
        return 1;
    }
    get_value(a, line, &a->lhs, &l);
    get_value(a, line, &a->rhs, &r);
    c = compare_values(&l, &r);
    switch (a->relop) {
    case REL_EQ: return c == 0;
    case REL_NE: return c != 0;
    case REL_LT: return c < 0;
    case REL_LE: return c <= 0;
    case REL_GT: return c > 0;
    default: return c >= 0;
    }
}

int sh_awk_apply(sh_awk_t *a, const char *line, sh_awk_emit_fn emit, void *ctx)
{
    int s, m;

    a->nr++;
    a->nf = 0;
    if (a->needs_fields && split_fields(a, line) != 0) {
        return -1;
    }
    m = pattern_matches(a, line);
    if (m < 0) {
        return -1;
    }
    if (m == 0) {
        return 0;
    }
    for (s = 0; s < a->nstmt; s++) {
        size_t used = 0;
        int k;

        for (k = 0; k < a->stmt[s].count; k++) {
            value_t v;

            get_value(a, line, &a->args[a->stmt[s].first + k], &v);
            if (v.len + (k > 0 ? 1 : 0) > SH_AWK_OUT_MAX - used) {
                errno = ERANGE;
                return -1;
            }
            if (k > 0) {
                a->out[used++] = ' ';
            }
            memcpy(a->out + used, v.s, v.len);
            used += v.len;
        }
        emit(ctx, a->out, used);
    }
    return 0;
}
