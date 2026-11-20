/* Bounded POSIX BRE matcher, no Vita APIs; notes in docs/shell-text-tools.md. */
#include "sh_grep.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#define OP_LIT 0  /* one byte in .lit */
#define OP_ANY 1  /* . */
#define OP_CLS 2  /* [abc], [!abc], [a-z] */
#define OP_CAT 3  /* .a then .b */
#define OP_ALT 4  /* .a or .b */
#define OP_REP 5  /* .a, between .min and .max times */
#define OP_BOL 6  /* ^ */
#define OP_EOL 7  /* $ */
#define OP_GRP 8  /* \( .a \) : brackets the body so a quantifier can hang off it */

#define CLASS_RANGES 24   /* ranges per bracket expression */
#define PROGRAM_MAX 512   /* nodes per compiled pattern */
#define GROUP_MAX 16      /* \( \) pairs per pattern */
#define INTERVAL_MAX 64   /* the widest interval \{m,n\} may ask for */
#define FRAME_MAX 256     /* pending (node, position) pairs during one match */
#define EDGE_MAX 4        /* continuations one node can lead to */
#define PROGRAM_MAX_BITS 10 /* enough to name every node index (2^10 = 1024) */

typedef struct {
    unsigned char lo, hi;
} grep_range_t;

typedef struct grep_node {
    unsigned char op;
    unsigned char icase;
    unsigned char negated;
    unsigned char ncls;
    unsigned char min, max;
    unsigned char lit;
    short a, b; /* operand nodes, -1 when unused */
    short next[EDGE_MAX];
    unsigned char copy_edge[EDGE_MAX];
    /* Marks the parallel next[] entry as finishing one copy of the enclosing repeat. */
    /* Where control goes once this node's text is consumed; a list, as one node can end twice. */
    int nnext;
    grep_range_t cls[CLASS_RANGES];
} node_t;

struct sh_grep_re {
    node_t *node;
    int nnodes;
    int root;
    int icase;
    int matches_all; /* the empty pattern: every line, no walk needed */
    int blank_only;  /* "^$": only an empty line */
    int anchored;    /* the pattern began with ^, so a match may start only at 0 */
    int literal;     /* no metacharacters at all, so sh_grep_find answers exactly */
    char *lit;
    size_t lit_len;
    char *text; /* the pattern as typed, for the error line */
};

/* Why the last compile failed; static since one server thread runs one command at a time. */
#define WHY_OK 0
#define WHY_BRACK 1  /* [ without a closing ] */
#define WHY_RANGE 2  /* [z-a], or more ranges than CLASS_RANGES */
#define WHY_PAREN 3  /* \( without \), or \) without \( */
#define WHY_ESC 4    /* a pattern that ends in a backslash */
#define WHY_BADRPT 5 /* a quantifier with nothing to quantify */
#define WHY_BRACE 6  /* \{ that is not a well-formed interval */
#define WHY_BADBR 7  /* an interval whose bounds do not fit */
#define WHY_TOOLONG 8 /* more program than PROGRAM_MAX */

static int g_why;

/* ------------------------------------------------------------------ */
/* byte helpers                                                         */
/* ------------------------------------------------------------------ */

char sh_grep_fold(int c)
{
    if (c >= 'A' && c <= 'Z') {
        return (char)(c + ('a' - 'A'));
    }
    return (char)c;
}

/* Reads one line through a byte callback: length, 0 at EOF, or -1 (line drained) if over cap. */
int sh_grep_read_line(sh_grep_fill_fn fill, void *ctx, char *line, size_t cap,
                      int *eof)
{
    size_t len = 0;
    char c;

    if (eof != NULL) {
        *eof = 0;
    }
    if (cap == 0) {
        return -1;
    }
    for (;;) {
        if (len + 1 >= cap) {
            while (fill(ctx, &c, 1) == 1 && c != '\n') {
                ;
            }
            return -1;
        }
        if (fill(ctx, &c, 1) != 1) {
            break;
        }
        if (c == '\n') {
            line[len] = '\0';
            return (int)len;
        }
        line[len++] = c;
    }
    if (len == 0) {
        /* No bytes and no newline is end of input; a blank line comes through above with len 0. */
        if (eof != NULL) {
            *eof = 1;
        }
        return 0;
    }
    line[len] = '\0';
    return (int)len;
}

/* Folded equality; icase folds ASCII only so bytes >= 0x80 compare as themselves, locale-free. */
static int byte_eq(unsigned char x, unsigned char y, int icase)
{
    if (x == y) {
        return 1;
    }
    return icase && (unsigned char)sh_grep_fold((int)x) ==
                    (unsigned char)sh_grep_fold((int)y);
}

/* ------------------------------------------------------------------ */
/* the substring search                                                 */
/* ------------------------------------------------------------------ */

/* Naive scan, not KMP: line and needle are bounded, so a failure table would cost more. */
const char *sh_grep_find(const char *haystack, const char *needle, size_t needle_len,
                         int icase)
{
    size_t i;

    if (needle_len == 0) {
        return haystack;
    }
    for (i = 0; haystack[i] != '\0'; i++) {
        size_t k;

        for (k = 0; k < needle_len; k++) {
            if (!byte_eq((unsigned char)haystack[i + k], (unsigned char)needle[k],
                         icase)) {
                break;
            }
        }
        if (k == needle_len) {
            return haystack + i;
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* bracket expressions                                                  */
/* ------------------------------------------------------------------ */

static int class_add(node_t *n, unsigned char lo, unsigned char hi)
{
    if (n->ncls >= CLASS_RANGES) {
        return -1;
    }
    n->cls[n->ncls].lo = lo;
    n->cls[n->ncls].hi = hi;
    n->ncls++;
    return 0;
}

/* Adds ch and its case counterpart as its own range, so [a-z] under -i also takes A-Z. */
static int class_add_char(node_t *n, int ch, int icase)
{
    /* Only the byte as written is stored; class_hit folds at match time. */
    (void)icase;
    return class_add(n, (unsigned char)ch, (unsigned char)ch);
}

static int class_hit(const node_t *n, unsigned char ch)
{
    int i, in = 0;

    /* With -i the test runs on folded bytes, accepting only a byte and its counterpart. */
    unsigned char f = n->icase ? (unsigned char)sh_grep_fold((int)ch) : ch;

    for (i = 0; i < n->ncls; i++) {
        if ((ch >= n->cls[i].lo && ch <= n->cls[i].hi) ||
            (n->icase && f >= n->cls[i].lo && f <= n->cls[i].hi)) {
            in = 1;
            break;
        }
    }
    return n->negated ? !in : in;
}

/* ------------------------------------------------------------------ */
/* the compiler                                                         */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *pat;
    size_t pos;
    node_t *node;
    int nnodes;
    int icase;
    int depth; /* \( \) pairs currently open */
} compiler_t;

static node_t *node_new(compiler_t *c, unsigned char op)
{
    node_t *n;

    if (c->nnodes >= PROGRAM_MAX) {
        g_why = WHY_TOOLONG;
        return NULL;
    }
    n = &c->node[c->nnodes++];
    memset(n, 0, sizeof(*n));
    n->op = op;
    n->a = n->b = -1;
    n->nnext = 0;
    return n;
}

static int class_parse(compiler_t *c, int idx)
{
    node_t *n = &c->node[idx];
    const char *p = c->pat + c->pos;
    int first = 1;

    /* POSIX BRE spells a complemented set [^abc]; the shell-style [!abc] is accepted too. */
    if (*p == '^' || *p == '!') {
        n->negated = 1;
        p++;
    }
    for (;;) {
        unsigned char lo;

        if (*p == '\0') {
            g_why = WHY_BRACK;
            return -1;
        }
        if (*p == ']' && !first) {
            p++;
            break;
        }
        first = 0;
        lo = (unsigned char)*p++;
        if (*p == '-' && p[1] != '\0' && p[1] != ']') {
            unsigned char hi;

            p++;
            hi = (unsigned char)*p++;
            if (hi < lo) {
                /* An empty set would let a typo'd range match nothing quietly. */
                g_why = WHY_RANGE;
                return -1;
            }
            if (class_add(n, lo, hi) != 0) {
                g_why = WHY_RANGE;
                return -1;
            }
            continue;
        }
        if (class_add_char(n, lo, c->icase) != 0) {
            g_why = WHY_RANGE;
            return -1;
        }
    }
    c->pos = (size_t)(p - c->pat);
    return 0;
}

static int parse_re(compiler_t *c, int *out);

/* Says control may pass from `node` to `next`; defines "finished" per node kind; -1 is a match. */
/* Like thread_tail, but a consume node landing on `owner` is recorded as a copy boundary. */
static void thread_body(compiler_t *c, int node, int owner);

static void thread_tail(compiler_t *c, int node, int next)
{
    node_t *n;
    int i;

    if (node < 0) {
        return;
    }
    n = &c->node[node];
    switch (n->op) {
    case OP_LIT:
    case OP_ANY:
    case OP_CLS:
        for (i = 0; i < n->nnext; i++) {
            if (n->next[i] == next) {
                return;
            }
        }
        if (n->nnext >= EDGE_MAX) {
            g_why = WHY_TOOLONG;
            return;
        }
        n->next[n->nnext] = (short)next;
        n->copy_edge[n->nnext] = 0;
        n->nnext++;
        break;
    case OP_BOL:
    case OP_EOL:
        /* An anchor consumes nothing, so control continues into its operand. */
        thread_tail(c, n->a, next);
        thread_tail(c, n->b, next);
        break;
    case OP_REP:
        for (i = 0; i < n->nnext; i++) {
            if (n->next[i] == next) {
                break;
            }
        }
        if (i == n->nnext) {
            if (n->nnext >= EDGE_MAX) {
                g_why = WHY_TOOLONG;
                return;
            }
            n->next[n->nnext++] = (short)next;
        }
        /* One more copy re-enters the body; `node` as continuation marks the copy-ending edges. */
        thread_body(c, n->a, node);
        break;
    case OP_CAT:
        /* The left operand's end is the right operand; the right's end is ours. */
        thread_tail(c, n->a, n->b);
        thread_tail(c, n->b, next);
        break;
    case OP_ALT:
        thread_tail(c, n->a, next);
        thread_tail(c, n->b, next);
        break;
    case OP_GRP:
        thread_tail(c, n->a, next);
        break;
    default:
        break;
    }
}

/* Reads decimal digits into val: 0 for none, 1 for a usable bound, -1 for too large a count. */
static int scan_count(const char *p, unsigned long *val, const char **end)
{
    const char *q = p;
    unsigned long v = 0;

    while (*q >= '0' && *q <= '9') {
        if (v > 255) {
            *end = q;
            return -1;
        }
        v = v * 10 + (unsigned long)(*q++ - '0');
    }
    *end = q;
    if (q == p) {
        return 0;
    }
    if (v > 255) {
        return -1;
    }
    *val = v;
    return 1;
}

/* One atom: a literal, the dot, a bracket expression, or a \( \) group. */
static int parse_atom(compiler_t *c, int *out)
{
    const char *p = c->pat + c->pos;
    int idx;

    if (*p == '\0') {
        g_why = WHY_BADRPT;
        return -1;
    }
    if (*p == '[') {
        c->pos++;
        idx = c->nnodes;
        if (node_new(c, OP_CLS) == NULL) {
            return -1;
        }
        c->node[idx].icase = (unsigned char)c->icase;
        if (class_parse(c, idx) != 0) {
            return -1;
        }
        *out = idx;
        return 0;
    }
    if (*p == '*') {
        /* Nothing to quantify; a literal star would answer a different question than was typed. */
        g_why = WHY_BADRPT;
        return -1;
    }
    if (*p == '\\') {
        if (p[1] == '\0') {
            g_why = WHY_ESC;
            return -1;
        }
        if (p[1] == '(') {
            int body, grp;

            c->pos += 2;
            if (c->depth >= GROUP_MAX) {
                g_why = WHY_TOOLONG;
                return -1;
            }
            c->depth++;
            if (parse_re(c, &body) != 0) {
                /* A group that never closes reports the parenthesis, not "nothing to repeat". */
                if (c->pat[c->pos] == '\0') {
                    g_why = WHY_PAREN;
                }
                return -1;
            }
            c->depth--;
            if (strncmp(c->pat + c->pos, "\\)", 2) != 0) {
                g_why = WHY_PAREN;
                return -1;
            }
            c->pos += 2;
            /* A group brackets its body so a quantifier can attach; it captures nothing, no \1. */
            grp = c->nnodes;
            if (node_new(c, OP_GRP) == NULL) {
                return -1;
            }
            c->node[grp].a = (short)body;
            *out = grp;
            return 0;
        }
        if (p[1] == ')') {
            g_why = WHY_PAREN;
            return -1;
        }
        /* \| \+ \? \{ \} \. \\ and any other \x stand for the byte itself. */
        c->pos += 2;
        idx = c->nnodes;
        if (node_new(c, OP_LIT) == NULL) {
            return -1;
        }
        c->node[idx].lit = (unsigned char)p[1];
        *out = idx;
        return 0;
    }
    if (*p == '.') {
        c->pos++;
        idx = c->nnodes;
        if (node_new(c, OP_ANY) == NULL) {
            return -1;
        }
        *out = idx;
        return 0;
    }
    /* ^ and $ away from the pattern ends are ordinary bytes; the ends were already stripped. */
    c->pos++;
    idx = c->nnodes;
    if (node_new(c, OP_LIT) == NULL) {
        return -1;
    }
    c->node[idx].lit = (unsigned char)*p;
    *out = idx;
    return 0;
}

/* An atom with any quantifier applied to it. */
static int parse_piece(compiler_t *c, int *out)
{
    const char *p;
    unsigned long lo = 0, hi = 0;
    int got_lo, got_hi = 0, comma = 0, bad = 0, rep;

    if (parse_atom(c, out) != 0) {
        return -1;
    }
    p = c->pat + c->pos;
    if (*p == '*') {
        lo = 0;
        hi = 255;
        c->pos++;
    } else if (strncmp(p, "\\+", 2) == 0) {
        lo = 1;
        hi = 255;
        c->pos += 2;
    } else if (strncmp(p, "\\?", 2) == 0) {
        lo = 0;
        hi = 1;
        c->pos += 2;
    } else if (strncmp(p, "\\{", 2) == 0) {
        const char *q = p + 2, *qe;

        got_lo = scan_count(q, &lo, &qe);
        if (got_lo < 0) {
            bad = 1;
        }
        q = qe;
        if (!bad && *q == ',') {
            comma = 1;
            q++;
            got_hi = scan_count(q, &hi, &qe);
            if (got_hi < 0) {
                bad = 1;
            }
            q = qe;
        }
            /* The closing brace is escaped too, so \{2\} ends at "\\}". */
            if (bad || got_lo == 0 || strncmp(q, "\\}", 2) != 0 ||
                (comma && got_hi && hi < lo)) {
                /* An unreadable \{ interval is an error, not a search for literal braces. */
                g_why = bad ? WHY_BADBR : WHY_BRACE;
                return -1;
            }
            if (!comma) {
                hi = lo;
            } else if (!got_hi) {
                hi = 255;
            }
            c->pos = (size_t)(q - c->pat) + 2;
    } else {
        return 0; /* no quantifier follows the atom */
    }

    if (hi > INTERVAL_MAX && hi != 255) {
        g_why = WHY_TOOLONG;
        return -1;
    }
    rep = c->nnodes;
    if (node_new(c, OP_REP) == NULL) {
        return -1;
    }
    c->node[rep].a = (short)*out;
    c->node[rep].min = (unsigned char)lo;
    c->node[rep].max = (unsigned char)hi;
    *out = rep;
    return 0;
}

static int parse_branch(compiler_t *c, int *out)
{
    int first, tail;

    if (parse_piece(c, &first) != 0) {
        return -1;
    }
    for (;;) {
        const char *p = c->pat + c->pos;
        int cat;

        if (*p == '\0' || strncmp(p, "\\|", 2) == 0 || strncmp(p, "\\)", 2) == 0) {
            break;
        }
        if (parse_piece(c, &tail) != 0) {
            return -1;
        }
        cat = c->nnodes;
        if (node_new(c, OP_CAT) == NULL) {
            return -1;
        }
        c->node[cat].a = (short)first;
        c->node[cat].b = (short)tail;
        /* The left operand's tail is the right operand; the right's tail is this CAT's own tail. */
        first = cat;
    }
    *out = first;
    return 0;
}

static int parse_re(compiler_t *c, int *out)
{
    int left, alt;

    if (parse_branch(c, &left) != 0) {
        return -1;
    }
    if (strncmp(c->pat + c->pos, "\\|", 2) != 0) {
        *out = left;
        return 0;
    }
    c->pos += 2;
    {
        int right;

        if (parse_re(c, &right) != 0) {
            return -1;
        }
        alt = c->nnodes;
        if (node_new(c, OP_ALT) == NULL) {
            return -1;
        }
        c->node[alt].a = (short)left;
        c->node[alt].b = (short)right;
        *out = alt;
    }
    return 0;
}

/* A pattern with no metacharacters or escapes can take the cheaper substring path. */
static int literal_setup(sh_grep_t *re, const char *pattern)
{
    const char *p;
    size_t len = strlen(pattern);
    char *buf;

    for (p = pattern; *p != '\0'; p++) {
        if (*p == '.' || *p == '[' || *p == '*' || *p == '^' || *p == '$' ||
            *p == '\\') {
            return 0;
        }
    }
    buf = malloc(len + 1);
    if (buf == NULL) {
        return -1;
    }
    memcpy(buf, pattern, len + 1);
    re->literal = 1;
    re->lit = buf;
    re->lit_len = len;
    return 0;
}

sh_grep_t *sh_grep_compile(const char *pattern, int flags)
{
    compiler_t c;
    sh_grep_t *re;
    int root = -1, body;
    size_t plen;

    g_why = WHY_OK;
    if (pattern == NULL) {
        errno = EINVAL;
        return NULL;
    }
    plen = strlen(pattern);
    if (plen > SH_GREP_PATTERN_MAX) {
        g_why = WHY_TOOLONG;
        errno = ERANGE;
        return NULL;
    }
    re = calloc(1, sizeof(*re));
    if (re == NULL) {
        errno = ENOMEM;
        return NULL;
    }
    re->text = malloc(plen + 1);
    if (re->text == NULL) {
        free(re);
        errno = ENOMEM;
        return NULL;
    }
    memcpy(re->text, pattern, plen + 1);
    re->icase = (flags & SH_GREP_ICASE) ? 1 : 0;

    if (strcmp(pattern, "^$") == 0) {
        re->blank_only = 1;
        return re;
    }
    if (plen == 0 || strcmp(pattern, "^") == 0 || strcmp(pattern, "$") == 0) {
        /* The empty pattern and the two degenerate anchors match every line. */
        re->matches_all = 1;
        return re;
    }

    /* Strip the anchors the compiler owns: leading ^ sets anchored, trailing $ becomes EOL. */
    char stripped[SH_GREP_PATTERN_MAX + 1];
    size_t slen = plen;

    if (slen >= 2 && pattern[slen - 1] == '$' && pattern[slen - 2] != '\\') {
        slen--;
    } else {
        /* No trailing anchor: the parser must see the whole pattern. */
        slen = plen;
    }
    memcpy(stripped, pattern, slen);
    stripped[slen] = '\0';

    memset(&c, 0, sizeof(c));
    c.pat = stripped;
    c.icase = re->icase;
    c.node = calloc(PROGRAM_MAX, sizeof(node_t));
    if (c.node == NULL) {
        free(re->text);
        free(re);
        errno = ENOMEM;
        return NULL;
    }

    /* A leading ^ fixes where a match begins; a trailing $ is an EOL node passable at line end. */
    re->anchored = (pattern[0] == '^');
    c.pos = re->anchored ? 1 : 0;
    if (parse_re(&c, &body) != 0) {
        int why = g_why;

        free(c.node);
        free(re->text);
        free(re);
        errno = (why == WHY_TOOLONG) ? ERANGE : EINVAL;
        return NULL;
    }
    if (c.pat[c.pos] != '\0' || c.depth != 0) {
        g_why = WHY_PAREN;
        free(c.node);
        free(re->text);
        free(re);
        errno = EINVAL;
        return NULL;
    }
    if (re->anchored) {
        int bol = c.nnodes;

        if (node_new(&c, OP_BOL) == NULL) {
            free(c.node);
            free(re->text);
            free(re);
            errno = ERANGE;
            return NULL;
        }
        c.node[bol].a = (short)body;
        root = bol;
    } else {
        root = body;
    }
    if (slen < plen) { /* a trailing $ was stripped above */
        int eol = c.nnodes, cat;

        if (node_new(&c, OP_EOL) == NULL) {
            free(c.node);
            free(re->text);
            free(re);
            errno = ERANGE;
            return NULL;
        }
        if (root < 0) {
            /* Nothing before the anchor, so EOL alone is the pattern. */
            root = eol;
        } else {
            cat = c.nnodes;
            if (node_new(&c, OP_CAT) == NULL) {
                free(c.node);
                free(re->text);
                free(re);
                errno = ERANGE;
                return NULL;
            }
            c.node[cat].a = (short)root;
            c.node[cat].b = (short)eol;
            root = cat;
        }
    }

    if (root < 0) {
        /* Nothing left to match so this matches all; only scratch is freed, the caller owns re. */
        re->matches_all = 1;
        re->node = NULL;
        re->nnodes = 0;
        re->root = -1;
        free(c.node);
        return re;
    }

    /* The whole pattern's end is nowhere: reaching it is a match. */
    thread_tail(&c, root, -1);
    if (g_why == WHY_TOOLONG) {
        free(c.node);
        free(re->text);
        free(re);
        errno = ERANGE;
        return NULL;
    }

    re->node = c.node;
    re->nnodes = c.nnodes;
    re->root = root;

    if (literal_setup(re, pattern) != 0) {
        free(re->node);
        free(re->text);
        free(re);
        errno = ENOMEM;
        return NULL;
    }
    return re;
}

void sh_grep_free(sh_grep_t *re)
{
    if (re == NULL) {
        return;
    }
    free(re->node);
    free(re->text);
    free(re->lit);
    free(re);
}

const char *sh_grep_compile_error(char *dst, size_t dstlen)
{
    static const char *const msg[] = {
        "", "unbalanced [", "invalid character range", "unbalanced \\( or \\)",
        "trailing backslash", "nothing to repeat", "invalid interval",
        "interval out of range", "pattern too large"
    };
    size_t n;

    if (dst == NULL || dstlen == 0) {
        return dst;
    }
    if (g_why <= WHY_OK || g_why > WHY_TOOLONG) {
        dst[0] = '\0';
        return dst;
    }
    n = strlen(msg[g_why]);
    if (n > dstlen - 1) {
        n = dstlen - 1;
    }
    memcpy(dst, msg[g_why], n);
    dst[n] = '\0';
    return dst;
}



static void thread_body(compiler_t *c, int node, int owner)
{
    node_t *n;
    int i;

    if (node < 0) {
        return;
    }
    n = &c->node[node];
    switch (n->op) {
    case OP_LIT:
    case OP_ANY:
    case OP_CLS:
        for (i = 0; i < n->nnext; i++) {
            if (n->next[i] == owner) {
                n->copy_edge[i] = 1;
                return;
            }
        }
        if (n->nnext >= EDGE_MAX) {
            g_why = WHY_TOOLONG;
            return;
        }
        n->next[n->nnext] = (short)owner;
        n->copy_edge[n->nnext] = 1;
        n->nnext++;
        break;
    case OP_CAT:
        /* Inside a body the left ends at the right uncharged; only the body's end charges. */
        thread_tail(c, n->a, n->b);
        thread_body(c, n->b, owner);
        break;
    case OP_ALT:
        thread_body(c, n->a, owner);
        thread_body(c, n->b, owner);
        break;
    case OP_GRP:
        thread_body(c, n->a, owner);
        break;
    case OP_BOL:
    case OP_EOL:
        thread_body(c, n->a, owner);
        thread_body(c, n->b, owner);
        break;
    case OP_REP:
        /* A nested repeat inside a body ends at the outer repeat too. */
        if (n->nnext < EDGE_MAX) {
            n->next[n->nnext] = (short)owner;
            n->copy_edge[n->nnext] = 1;
            n->nnext++;
        }
        thread_body(c, n->a, node);
        break;
    default:
        break;
    }
}

/* ------------------------------------------------------------------ */
/* matching                                                             */
/* ------------------------------------------------------------------ */

/* Thompson simulation over the compiled tree: the live set is deduplicated, so cost is bounded. */

#define SET_SLOTS 2048  /* open-addressed live-set slots: a power of two, > 2 * PROGRAM_MAX */
#define LIVE_PAIRS 1024 /* live states at one position: PROGRAM_MAX nodes x a few counts */

typedef struct {
    const sh_grep_t *re;
    const char *s;
    size_t len;
    unsigned int *slots;
    unsigned int stamp; /* the position: clearing the set is one increment */
    int *pending;
    int npending;
    int *live;          /* every state reached at this position, in arrival order */
    int nlive;
    long steps;
    long budget;      /* expansions allowed for this line: see SH_GREP_STEP_LIMIT */
    int failed;
    int hit;
} matcher_t;

/* A live state is node, repeat count and position packed in one key; 0 alone means unused. */
#define STAMP_WRAP (1u << 13)

static unsigned int state_key(int node, int count, unsigned int stamp)
{
    /* node + 1 keeps a live state non-zero even at count 0 in slot node 0. */
    return ((unsigned int)stamp << (PROGRAM_MAX_BITS + 9)) |
           (((unsigned int)node + 1u) << 8) | ((unsigned int)count & 0xFFu);
}

static void set_put(matcher_t *m, int node, int count)
{
    unsigned int key = state_key(node, count, m->stamp);
    size_t h = (size_t)(key * 2654435761u) & (size_t)(SET_SLOTS - 1);
    size_t i;

    for (i = 0; i < SET_SLOTS; i++) {
        if (m->slots[h] == 0 || m->slots[h] == key) {
            m->slots[h] = key;
            return;
        }
        h = (h + 1) & (size_t)(SET_SLOTS - 1);
    }
}

static int set_has(const matcher_t *m, int node, int count)
{
    unsigned int key = state_key(node, count, m->stamp);
    size_t h = (size_t)(key * 2654435761u) & (size_t)(SET_SLOTS - 1);
    size_t i;

    for (i = 0; i < SET_SLOTS; i++) {
        if (m->slots[h] == 0) {
            return 0;
        }
        if (m->slots[h] == key) {
            return 1;
        }
        h = (h + 1) & (size_t)(SET_SLOTS - 1);
    }
    return 0;
}

static void push_state(matcher_t *m, int node, int count)
{
#ifdef SH_GREP_TRACE
    fprintf(stderr, "push node=%d cnt=%d stamp=%u\n", node, count, m->stamp);
#endif
    if (node < 0) {
        /* Nothing left to match: the whole pattern has been consumed, which is the answer. */
        m->hit = 1;
        return;
    }
    if (++m->steps > m->budget) {
        m->failed = 1;
        return;
    }
    if (node >= 0) {
        const node_t *rn = &m->re->node[node];

        /* A consume node keeps its count; only an unbounded repeat's count clamps at its min. */
        if (rn->op == OP_REP && rn->max == 255 && count > (int)rn->min) {
            count = (int)rn->min;
        }
        if (count < 0) {
            count = 0;
        }
    }
    if (set_has(m, node, count)) {
        return;
    }
    set_put(m, node, count);
    if (m->npending + 1 < 2 * LIVE_PAIRS) {
        m->pending[m->npending++] = node;
        m->pending[m->npending++] = count;
    }
    if (m->nlive + 1 < 2 * LIVE_PAIRS) {
        m->live[m->nlive] = node;
        m->live[m->nlive + 1] = count;
        m->nlive += 2;
    }
}

/* True when node can match the empty string, so a CAT's right operand is live at the left's end. */
static int node_can_be_empty(const sh_grep_t *re, int node)
{
    const node_t *n;

    if (node < 0) {
        return 1;
    }
    n = &re->node[node];
    switch (n->op) {
    case OP_CAT:
        return node_can_be_empty(re, n->a) && node_can_be_empty(re, n->b);
    case OP_ALT:
        return node_can_be_empty(re, n->a) || node_can_be_empty(re, n->b);
    case OP_GRP:
        return node_can_be_empty(re, n->a);
    case OP_REP:
        return n->min == 0 || node_can_be_empty(re, n->a);
    case OP_BOL:
        return node_can_be_empty(re, n->a); /* ^ itself consumes nothing, its body might */
    case OP_EOL:
        return 1; /* passable without consuming */
    default:
        return 0; /* a literal, dot or class consumes one byte */
    }
}

/* Seeds node and closes over every edge that costs no byte; `at` is needed only by anchors. */
static void closure(matcher_t *m, int node, int count, size_t at)
{
    push_state(m, node, count);
    while (m->npending > 0 && !m->failed) {
        int cnt = m->pending[--m->npending];
        int idx = m->pending[--m->npending];
        const node_t *n = &m->re->node[idx];

        switch (n->op) {
        case OP_CAT:
            /* The left is live; so is the right when the left can match nothing (`se*` takes e). */
            push_state(m, n->a, cnt);
            if (node_can_be_empty(m->re, n->a)) {
                push_state(m, n->b, cnt);
            }
            break;
        case OP_ALT:
            push_state(m, n->a, cnt);
            push_state(m, n->b, cnt);
            break;
        case OP_GRP:
            /* A group is a bracket only: an enclosing repeat's copy count passes through it. */
            push_state(m, n->a, cnt);
            break;
        case OP_REP: {
            int left = cnt < (int)n->min;

            /* Entering the repeat enters the body; a finished copy returns here with cnt set. */
            if (n->max == 255 || cnt < (int)n->max) {
                /* Starting another copy keeps the count; an unbounded repeat clamps it to min. */
                int ncnt = (n->max == 255 && cnt > (int)n->min) ? (int)n->min
                                                                 : cnt;

                push_state(m, n->a, ncnt);
            }
            if (!left) {
                /* The minimum is satisfied, so the repeat may be left. */
                int j;

                for (j = 0; j < n->nnext; j++) {
                    push_state(m, n->next[j], 0);
                }
            }
            break;
        }
        case OP_BOL:
            if (at == 0) {
                push_state(m, n->a, 0);
            }
            break;
        case OP_EOL:
            /* $ passes only at end of line, then continues into its operand, the pattern's tail. */
            if (at == m->len) {
                push_state(m, n->a, 0);
                push_state(m, n->b, 0);
            }
            break;
        default:
            break;
        }
    }
}

/* Seeds the next position with the states the consumed byte leads to. */
static void closure_seeds(matcher_t *m, const int *seed, int n, size_t at)
{
    int i;

    for (i = 0; i + 1 < n; i += 2) {
        closure(m, seed[i], seed[i + 1], at);
    }
}

/* Search mode (best NULL) answers whether any match exists. Span mode starts a match at `start`
 * only and records in *best the end of the longest one, -1 when none. */
static int walk(const sh_grep_t *re, const char *line, size_t len, unsigned int *slots,
                int *pending, int *live, size_t start, long *best)
{
    matcher_t m;
    size_t pos;
    int i;

    m.re = re;
    m.s = line;
    m.len = len;
    m.slots = slots;
    m.pending = pending;
    m.live = live;
    m.stamp = 1;
    m.npending = 0;
    m.nlive = 0;
    m.steps = 0;
    m.budget = SH_GREP_STEP_LIMIT(len);
    m.failed = 0;
    m.hit = 0;

    if (best != NULL) {
        *best = -1;
    }
    closure(&m, re->root, 0, start);
    if (m.failed) {
        errno = EAGAIN;
        return -1;
    }
    if (m.hit) {
        if (best == NULL) {
            return 1;
        }
        *best = (long)start;
        m.hit = 0;
    }
    for (pos = start; pos < len; pos++) {
        unsigned char ch = (unsigned char)line[pos];
        int seed[2 * LIVE_PAIRS];
        int nseed = 0;

        /* Walk only live states, not every (node, count) pair, so cost follows reachable states. */
        for (i = 0; i + 1 < m.nlive; i += 2) {
            int node = m.live[i];
            int cnt = m.live[i + 1];
            const node_t *n = &re->node[node];

            if (n->op != OP_LIT && n->op != OP_ANY && n->op != OP_CLS) {
                continue;
            }
            if (n->op == OP_LIT && !byte_eq(ch, n->lit, re->icase)) {
                continue;
            }
            if (n->op == OP_CLS && !class_hit(n, ch)) {
                continue;
            }
            {
                int j;

                for (j = 0; j < n->nnext; j++) {
                    /* Reaching the enclosing repeat charges the count; other edges keep it. */
                    int ncnt = n->copy_edge[j] ? cnt + 1 : cnt;

                    if (nseed + 1 < 2 * LIVE_PAIRS) {
                        seed[nseed++] = n->next[j];
                        seed[nseed++] = ncnt;
                    }
                }
            }
        }
        if (best != NULL && nseed == 0) {
            break; /* no state is left to extend, so the longest match is already known */
        }
        if (best == NULL && !re->anchored) {
            /* A search may begin at any byte, so the entry is seeded at every position unless ^. */
            if (nseed + 1 < 2 * LIVE_PAIRS) {
                seed[nseed++] = re->root;
                seed[nseed++] = 0;
            }
        }
        m.stamp++;
        if (m.stamp >= STAMP_WRAP) {
            memset(m.slots, 0, (size_t)SET_SLOTS * sizeof(*m.slots));
            m.stamp = 1;
        }
        /* The set covers only the current byte, so it is emptied or stale keys fill the table. */
        memset(m.slots, 0, (size_t)SET_SLOTS * sizeof(*m.slots));
        m.npending = 0;
        m.nlive = 0;
        closure_seeds(&m, seed, nseed, pos + 1);
        if (m.failed) {
            errno = EAGAIN;
            return -1;
        }
        if (m.hit) {
            if (best == NULL) {
                return 1;
            }
            *best = (long)(pos + 1);
            m.hit = 0;
        }
    }
    return best != NULL ? *best >= 0 : m.hit;
}

int sh_grep_match(const sh_grep_t *re, const char *line, int invert)
{
    unsigned int *slots;
    int *pending;
    int *live;
    int hit;

    if (re == NULL || line == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (re->blank_only) {
        hit = line[0] == '\0';
        return invert ? !hit : hit;
    }
    if (re->matches_all) {
        return invert ? 0 : 1;
    }
    if (re->literal) {
        hit = sh_grep_find(line, re->lit, re->lit_len, re->icase) != NULL;
        return invert ? !hit : hit;
    }
    slots = calloc(SET_SLOTS, sizeof(*slots));
    pending = calloc(2 * LIVE_PAIRS, sizeof(*pending));
    live = calloc(2 * LIVE_PAIRS, sizeof(*live));
    if (slots == NULL || pending == NULL || live == NULL) {
        free(slots);
        free(pending);
        free(live);
        errno = ENOMEM;
        return -1;
    }
    hit = walk(re, line, strlen(line), slots, pending, live, 0, NULL);
    free(slots);
    free(pending);
    free(live);
    if (hit < 0) {
        return -1;
    }
    return invert ? !hit : hit;
}

int sh_grep_span(const sh_grep_t *re, const char *line, size_t from, size_t *so, size_t *eo)
{
    size_t len, start;
    unsigned int *slots;
    int *pending;
    int *live;
    long best = -1;
    int r = 0;

    if (re == NULL || line == NULL || so == NULL || eo == NULL) {
        errno = EINVAL;
        return -1;
    }
    len = strlen(line);
    if (from > len) {
        return 0;
    }
    if (re->blank_only) {
        *so = *eo = 0;
        return len == 0 && from == 0;
    }
    if (re->matches_all) {
        /* Empty at from for "", only at the start for "^", only at the end for "$". */
        size_t at = re->text[0] == '^' ? 0 : re->text[0] == '$' ? len : from;

        if (at < from) {
            return 0;
        }
        *so = *eo = at;
        return 1;
    }
    if (re->literal) {
        const char *at = sh_grep_find(line + from, re->lit, re->lit_len, re->icase);

        if (at == NULL) {
            return 0;
        }
        *so = (size_t)(at - line);
        *eo = *so + re->lit_len;
        return 1;
    }
    slots = calloc(SET_SLOTS, sizeof(*slots));
    pending = calloc(2 * LIVE_PAIRS, sizeof(*pending));
    live = calloc(2 * LIVE_PAIRS, sizeof(*live));
    if (slots == NULL || pending == NULL || live == NULL) {
        free(slots);
        free(pending);
        free(live);
        errno = ENOMEM;
        return -1;
    }
    /* The leftmost start that matches wins; at that start the longest end wins. */
    for (start = from; start <= len; start++) {
        memset(slots, 0, (size_t)SET_SLOTS * sizeof(*slots));
        r = walk(re, line, len, slots, pending, live, start, &best);
        if (r != 0) {
            break;
        }
    }
    free(slots);
    free(pending);
    free(live);
    if (r > 0) {
        *so = start;
        *eo = (size_t)best;
    }
    return r;
}

#ifdef SH_GREP_DEBUG
void sh_grep_debug_dump(const sh_grep_t *re)
{
    int i, j;

    for (i = 0; i < re->nnodes; i++) {
        const node_t *n = &re->node[i];

        printf("node %d op=%d a=%d b=%d min=%d max=%d lit=%c nnext=%d next=[", i, n->op,
               n->a, n->b, n->min, n->max, (n->lit >= 32 && n->lit < 127) ? n->lit : '.',
               n->nnext);
        for (j = 0; j < n->nnext; j++) {
            printf("%d/%d,", n->next[j], n->copy_edge[j]);
        }
        printf("]\n");
    }
    printf("root=%d\n", re->root);
}
#endif
