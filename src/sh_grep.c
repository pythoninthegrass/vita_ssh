/* A bounded POSIX basic regex matcher with no Vita APIs: the SDK declares regcomp in
 * <regex.h> but no library in its sysroot defines it. */
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
    /* Marks the parallel entry in next[] as "one more copy of the enclosing repeat is
     * finished", which is the only way the walk can charge a repeat whose body is a
     * CAT or a group: its last state is several nodes below the repeat's operand. */
    /* Where control goes once this node's own text is consumed. A list, not one slot,
     * because the same node can finish in more than one place: a repeat's body ends
     * both back at the repeat and at whatever follows the repeat. */
    int nnext;
    grep_range_t cls[CLASS_RANGES];
} node_t;

struct sh_grep_re {
    node_t *node;
    int nnodes;
    int root;
    int icase;
    int matches_all; /* the empty pattern: every line, no walk needed */
    int anchored;    /* the pattern began with ^, so a match may start only at 0 */
    int literal;     /* no metacharacters at all, so sh_grep_find answers exactly */
    char *lit;
    size_t lit_len;
    char *text; /* the pattern as typed, for the error line */
};

/* Why the last compile failed. Static because there is one server thread running one
 * command at a time, and a failed compile has to name its cause without making every
 * caller thread a context through. */
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

/*
 * Reads one line through a byte callback, so the bounded line rule lives with the
 * matcher rather than in whichever builtin is reading. fill() copies up to want bytes
 * into p and returns how many it took, or 0 at end of input; grep, and later wc and
 * sed, supply a file descriptor or the session's stdin behind it. A line is returned
 * without its newline. The return is the length, 0 at end of input, or -1 for a line
 * longer than cap -- which is drained to the next newline first, so the following read
 * starts clean and the caller gets exactly one overlong verdict per long line. Truncating
 * instead would match a prefix the caller never asked about.
 */
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
        /* Nothing read and no newline: this is end of input, not a blank line. A blank
         * line arrives through the newline branch above with len 0 and eof clear, so
         * the caller's loop keeps going across blank lines instead of stopping at the
         * first one. */
        if (eof != NULL) {
            *eof = 1;
        }
        return 0;
    }
    line[len] = '\0';
    return (int)len;
}

/* Folded equality. icase folds ASCII only, so a byte >= 0x80 compares as itself; there
 * is no locale table here, and consulting one would make -i answer differently for the
 * same bytes on a different host. */
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

/* Naive scan, not KMP. A line is bounded by SH_GREP_LINE_MAX and a needle by
 * SH_GREP_PATTERN_MAX, so the worst case is a bounded number of byte comparisons in a
 * buffer that already lives in cache; a failure table would cost more than the
 * comparisons it saves at this size. */
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

/* Adds ch and, when case is folded away, its counterpart, so [a-z] under -i also takes
 * A-Z. The counterpart is its own range rather than a widened one, because widening
 * would swallow the punctuation between the two halves of ASCII. */
static int class_add_char(node_t *n, int ch, int icase)
{
    /* Only the byte as written is stored; class_hit folds at match time, which covers
     * both cases without a second range for each one. icase is unused here and kept in
     * the signature so every range goes through one function. */
    (void)icase;
    return class_add(n, (unsigned char)ch, (unsigned char)ch);
}

static int class_hit(const node_t *n, unsigned char ch)
{
    int i, in = 0;

    /* With -i the test is done on folded bytes rather than by storing each range
     * twice: a range typed in either case then matches the other, and a negated set
     * stays the complement of exactly the set that was written. The folded endpoints
     * bound a small extra span of punctuation, so both a byte and its counterpart are
     * tested explicitly and only those two are ever accepted. */
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

    /* POSIX BRE spells a complemented set [^abc]; the shell-style [!abc] is accepted
     * too, since people reach for it and answering a different question than the one
     * typed is worse than being generous about it. */
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

/* Says that once `node` has finished, control may pass to `next`, recursing into the
 * operands that can themselves be the end of a larger construct. This is the only place
 * that knows what "finished" means for each node kind, so the walk never has to ask who
 * called it -- which is what makes a state reached by two routes one state, explored
 * once. -1 means the end of the whole pattern, i.e. a match. */
/* Threads the inside of a repeat's body. Identical to thread_tail except that a
 * consume node landing on `owner` -- the repeat -- is recorded as a copy boundary, which
 * is the only way the walk can tell "this copy is finished" from "about to start the
 * first one". */
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
        /* An anchor consumes nothing, so control simply continues into its operand.
         * It carries no continuation list of its own. */
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
        /* One more copy re-enters the body, and the body's own end comes back to this
         * repeat. Threading it with `node` as the continuation marks the edges that
         * complete a copy, which is what the walk charges; an edge into this repeat
         * from a preceding sibling is threaded by the CAT case above and stays
         * uncharged, so `ab\?c` does not credit the `a` as a copy of `b`. */
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

/* Reads decimal digits into val. Returns 0 for none, 1 for a usable bound, -1 for a
 * value too large to be a repeat count. */
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
        /* Nothing to quantify. Reading it as a literal star would answer a different
         * question than the one that was asked. */
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
                /* A group that never closes reports the parenthesis, not whatever the
                 * empty remainder happened to look like: `grep a\(` is a mistyped
                 * group, and "nothing to repeat" sends the reader looking for a
                 * quantifier that is not there. */
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
            /* A group brackets its body, which is what makes it a unit a quantifier
             * can be hung off. It captures nothing and \1 is no backreference: grep
             * asks only whether a line matches, so there is nothing to hand back. */
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
    /* ^ and $ away from the ends of the pattern are ordinary bytes, which is how BRE
     * defines them. sh_grep_compile has already taken the ends: it strips a leading ^
     * and a trailing $ from the text the parser sees, so neither reaches here as an
     * anchor. */
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
                /* In BRE a \{ that cannot be read as an interval is a literal brace, but
                 * a mistyped \{2,1\} that quietly searches for braces instead of
                 * reporting the bounds is the wrong answer to the question that was
                 * asked. */
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
        /* The left operand's tail is the right operand, and the right operand's tail is
         * whatever this CAT's own tail turns out to be; the walk reads b for the first
         * and accept for the second, so the chain is complete without a patch pass. */
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

/* A pattern with no metacharacters at all: substring search and the tree agree
 * exactly, so the caller may take the cheaper path. An escape rules it out, because
 * then the bytes the matcher sees are not the bytes that were typed. */
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

    if (plen == 0 || strcmp(pattern, "^") == 0 || strcmp(pattern, "$") == 0) {
        /* The empty pattern matches every line, and so do the two degenerate anchors:
         * every line has a start and an end. */
        re->matches_all = 1;
        return re;
    }

    /* Strip the anchors the compiler owns so the parser never sees them as bytes:
     * a leading ^ becomes the anchored flag and a trailing $ the EOL node below. */
    char stripped[SH_GREP_PATTERN_MAX + 1];
    size_t slen = plen;

    if (slen >= 2 && pattern[slen - 1] == '$' && pattern[slen - 2] != '\\') {
        slen--;
    } else {
        /* No trailing anchor: the parser must see the whole pattern. */
        slen = plen;
    }
    if (slen == 0 || (slen == 1 && pattern[0] == '^')) {
        /* "^", "$", "^$": every line has a start and an end. */
        re->matches_all = 1;
        return re;
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

    /* A leading ^ constrains where a match may begin; a trailing $ becomes an EOL node
     * the walk may pass only at the line's end. Elsewhere both are ordinary bytes. */
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
        /* Nothing left to match: every line has a start, so this matches all. The
         * caller still owns the object, so only the scratch program is released --
         * freeing re here would hand the caller a dangling handle to match against. */
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
        /* Inside a body the left operand still ends at the right one, uncharged; only
         * the body's true end reaches the repeat. */
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

/* Thompson's algorithm over the compiled tree. At each byte the matcher holds the set
 * of (node, position-in-a-repeat) pairs the pattern could be in, and advances them
 * together. A pair reached twice is added once, so no alternative is explored twice and
 * the cost is the line times the nodes reachable at a position -- bounded by the
 * program, never by the shape of the pattern. That is the property this needs: the
 * matcher runs in the server's one poll loop, where a pattern whose cost depends on its
 * own shape, such as \(a*\)*b over a few hundred bytes, would hold every session's
 * socket the way a stalled socket write does.
 *
 * Walking the tree instead of a pre-flattened state graph is what keeps the compiler
 * honest: a node's continuation is passed in when it is reached, so a CAT, a group and
 * a repeat can each say where their tail goes without a patch-up pass that could leave
 * a hole. The budget in SH_GREP_STEP_LIMIT is the backstop, charged once per node
 * visited; a pattern that somehow outgrows it answers EAGAIN rather than guessing "no
 * match", because an admitted failure is information and a wrong answer is not.
 *
 * The walk is iterative over an explicit pending list, not recursive, because it runs
 * on the server thread whose stack is the fixed SERVER_STACK handed to
 * sceKernelCreateThread. */

#define SET_SLOTS 2048  /* open-addressed live-set slots: a power of two, > 2 * PROGRAM_MAX */
#define LIVE_PAIRS 1024 /* states that can be live at one position: PROGRAM_MAX nodes,
                         * each reachable at a handful of repeat counts before the step
                         * budget ends the attempt anyway */

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

/* A live state is a node plus how far through a repeat it is, tagged with the position
 * so that advancing is one increment rather than a clear. The three fields are laid out
 * so that no two distinct states can share a key, and 0 alone means "slot unused" -- a
 * bias into the count's low bit would make copies 0 and 1 of a repeat indistinguishable,
 * which is exactly how a\{2\} comes to accept a single character. PROGRAM_MAX nodes and
 * counts to 255 take 10 and 8 bits; the node index starts at bit 9 so that count 0 with
 * node 0 still leaves the whole word zero. */
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
        /* Nothing left to match: the whole pattern has been consumed, which is the
         * answer. */
        m->hit = 1;
        return;
    }
    if (++m->steps > m->budget) {
        m->failed = 1;
        return;
    }
    if (node >= 0) {
        const node_t *rn = &m->re->node[node];

        /* A count is a property of a repeat, and it rides through the nodes between
         * the repeat and the byte that ends a copy -- so a consume node must keep the
         * count it is handed, not lose it, or `a\{2\}` can never finish its second
         * copy. Only an unbounded repeat's own count is reduced: past its minimum one
         * copy and a hundred are the same state, and clamping there is what keeps a
         * nested pair of stars (\(a*\)*) from holding a live state per copy count and
         * exhausting the budget on a no-match every real grep answers at once. */
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

/* True when node can match the empty string, which is what decides whether a CAT's
 * right operand is already live at the left's position. Recursive over the tree, whose
 * depth is bounded by GROUP_MAX and the pattern length, and memo-free because a pattern
 * is short; correctness first, and the walk's own step budget bounds the cost. */
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
    case OP_EOL:
        return 1; /* passable without consuming */
    default:
        return 0; /* a literal, dot or class consumes one byte */
    }
}

/* Seeds node and closes over every edge that costs no byte. `at` is the byte position,
 * which the anchors need and nothing else does. */
static void closure(matcher_t *m, int node, int count, size_t at)
{
    push_state(m, node, count);
    while (m->npending > 0 && !m->failed) {
        int cnt = m->pending[--m->npending];
        int idx = m->pending[--m->npending];
        const node_t *n = &m->re->node[idx];

        switch (n->op) {
        case OP_CAT:
            /* The left operand is what the pattern is trying now. The right one is
             * also live whenever the left can match nothing -- which is exactly when
             * the left's subtree contains a repeat whose minimum is zero -- because
             * then the pattern is already at the right operand at this same position.
             * Seeding only the left is the classic mistake that makes `se*` refuse
             * `e`: the s matches zero bytes, so the pattern is at `e*` immediately. */
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
            /* A group is a bracket only: the copy count of an enclosing repeat passes
             * through it, because the group's body is the repeat's body and its last
             * consume state is what charges the copy. Resetting here would make
             * \(a\)\{2\} never finish a second copy. */
            push_state(m, n->a, cnt);
            break;
        case OP_REP: {
            int left = cnt < (int)n->min;

            /* Entering the repeat enters the body. A copy that has consumed its body
             * arrives here again through the body's accept, which is the repeat
             * itself, so cnt is the copy already finished. */
            if (n->max == 255 || cnt < (int)n->max) {
                /* Starting another copy: the count stays where it is, because this
                 * copy is in progress rather than finished. It is the body's own
                 * tail, arriving back here, that charges the copy.
                 *
                 * An unbounded repeat's count only has to reach its minimum: past
                 * that, one copy and a hundred are the same state, so the count is
                 * clamped to min. Without this a nested pair of stars -- \(a*\)* --
                 * keeps a live state per (inner, outer) count pair, which is quadratic
                 * in the line and exhausts the budget on a no-match that every real
                 * grep answers at once. */
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
            /* $ is satisfied only at the end of the line, and passing it goes onward
             * through its operand -- the EOL node is placed after the body by
             * sh_grep_compile, so its operand is the pattern's tail. */
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

static int walk(const sh_grep_t *re, const char *line, size_t len, unsigned int *slots,
                int *pending, int *live)
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

    closure(&m, re->root, 0, 0);
    if (m.failed) {
        errno = EAGAIN;
        return -1;
    }
    if (m.hit) {
        return 1;
    }
    for (pos = 0; pos < len; pos++) {
        unsigned char ch = (unsigned char)line[pos];
        int seed[2 * LIVE_PAIRS];
        int nseed = 0;

        /* Walk the states that are actually live, not every (node, count) pair: a
         * pattern with a repeat has 256 possible counts per node and only a few are
         * ever reachable, so scanning them all would make the cost grow with the
         * pattern's counts instead of with its reachable states -- the superlinear
         * behaviour that made a few hundred bytes of `a` exhaust the step budget. */
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
                    /* Reaching the enclosing repeat ends a copy, so that edge charges
                     * the count. Every other edge -- into the next byte of the same
                     * copy, or onward past a finished repeat -- keeps the count the
                     * state already had, because nothing has been completed or begun
                     * afresh. A charged count past an unbounded repeat's minimum is
                     * the same state as the minimum, so it is clamped: this is what
                     * stops \(a*\)* from holding a live state per copy count. */
                    int ncnt = n->copy_edge[j] ? cnt + 1 : cnt;

                    if (nseed + 1 < 2 * LIVE_PAIRS) {
                        seed[nseed++] = n->next[j];
                        seed[nseed++] = ncnt;
                    }
                }
            }
        }
        if (!re->anchored) {
            /* A search may begin at any byte, so the pattern's entry is live at every
             * position, not only at 0; a leading ^ is what takes this away. Appended
             * to the same seed list so the whole position is closed at once -- closing
             * the byte-consumed seeds first and the restart second lets a repeat's
             * body be reached by both routes and consume the same byte twice. */
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
        /* The set answers only about the current byte, so it is emptied here: keys carry
         * the position too, but stale keys still hold slots and the table would fill
         * after a few hundred bytes. */
        memset(m.slots, 0, (size_t)SET_SLOTS * sizeof(*m.slots));
        m.npending = 0;
        m.nlive = 0;
        closure_seeds(&m, seed, nseed, pos + 1);
        if (m.failed) {
            errno = EAGAIN;
            return -1;
        }
        if (m.hit) {
            return 1;
        }
    }
    return m.hit;
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
    hit = walk(re, line, strlen(line), slots, pending, live);
    free(slots);
    free(pending);
    free(live);
    if (hit < 0) {
        return -1;
    }
    return invert ? !hit : hit;
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
