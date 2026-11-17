/* Host test for src/sh_textops.c; driven by scripts/test_sh_textops.py.
 *
 * The references are naive line loops written from the specification, not from the
 * implementation, so agreement between the two says something. The Python wrapper pins
 * those references against a handful of real gwc/gsort/guniq calls before running this,
 * so a wrong reference is caught rather than copied. Everything is in memory: the
 * exhaustive part is every string up to length 6 over {a, b, newline, space, 1, -} for
 * every flag combination, with no subprocesses per case. */
#include "sh_textops.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
static unsigned long ticks; /* the watchdog below prints the case it is stuck on */

static void tick(const char *what, size_t len)
{
    if (++ticks % 4000000UL != 0) {
        return;
    }
    printf("PROGRESS %lu cases, last %s len=%zu\n", ticks, what, len);
    fflush(stdout);
}

#define CHECK(cond)                                       \
    do {                                                  \
        if (!(cond)) {                                    \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, \
                   #cond);                                \
            failures++;                                   \
        }                                                 \
    } while (0)

#define OUT_CAP 65536
#define ALPHA "ab\n 1-"
#define MAX_LEN 6
#define REF_MAX 64

/* File scope: a 64 KB answer buffer does not belong on a thread stack, and the harness
 * runs on one. */
static char got_buf[OUT_CAP];
static char ref_buf[OUT_CAP];

/* --- the input, behind the fill callback a real source uses --- */

typedef struct {
    const char *p;
    size_t len;
    size_t off;
    size_t fail_at; /* SIZE_MAX to never fail a read */
} mem_src_t;

static ssize_t mem_fill(void *vctx, char *p, size_t want)
{
    mem_src_t *src = vctx;
    size_t left;

    if (src->off >= src->fail_at) {
        errno = EIO;
        return -1;
    }
    if (src->off >= src->len) {
        return 0;
    }
    left = src->len - src->off;
    if (want > left) {
        want = left;
    }
    if (want > 7) { /* short fills exercise the chunked reader */
        want = 7;
    }
    memcpy(p, src->p + src->off, want);
    src->off += want;
    return (ssize_t)want;
}

static void src_init(mem_src_t *src, const char *p, size_t len)
{
    src->p = p;
    src->len = len;
    src->off = 0;
    src->fail_at = SIZE_MAX;
}

/* --- the answer, behind the sink callback the tools print through --- */

typedef struct {
    char *buf;
    size_t len;
    size_t cap;
    int refuse;   /* nonzero: the sink stops the tool, as a dead channel would */
    int with_count; /* nonzero: uniq -c, so the row is prefixed with the run count */
} sink_t;

static void sink_init(sink_t *s, char *buf, size_t cap)
{
    s->buf = buf;
    s->len = 0;
    if (buf != NULL && cap > 0) {
        buf[0] = '\0'; /* so a caller may strcmp() the answer after a run that wrote none */
    }
    s->cap = cap;
    s->refuse = 0;
    s->with_count = 0;
}

static int sink_put(sink_t *s, const char *p, size_t n)
{
    if (n > s->cap || s->len > s->cap - n || s->refuse) {
        return 1; /* the answer outgrew the buffer, or the caller asked us to stop */
    }
    memcpy(s->buf + s->len, p, n);
    s->len += n;
    return 0;
}

/* The sink appends the newline that every line the tools print is followed by, so an
 * input whose last line had none still comes back terminated. */
static int sink_line(void *vctx, const char *line, size_t len, unsigned long count)
{
    sink_t *s = vctx;
    char head[16];
    int n;

    if (s->with_count) {
        /* uniq -c prints the count right-aligned in seven, then one space. */
        n = snprintf(head, sizeof(head), "%7lu ", count);
        if (n < 0 || sink_put(s, head, (size_t)n) != 0) {
            return 1;
        }
    }
    if (sink_put(s, line, len) || sink_put(s, "\n", 1)) {
        return 1;
    }
    if (s->len < s->cap) {
        s->buf[s->len] = '\0'; /* so a caller may strcmp() the answer it was handed */
    }
    return 0;
}

/* --- the naive references, written from the specification --- */

static int ref_blank(int c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r';
}

/* A line ends at a newline, or at end of input when the last line has none; the empty
 * input has no lines and "\n" has one empty line. */
static size_t ref_split(const char *b, size_t len, size_t *st, size_t *ln, size_t cap)
{
    size_t n = 0;
    size_t start = 0;
    size_t i;

    for (i = 0; i < len; i++) {
        if (b[i] == '\n') {
            if (n == cap) {
                return n;
            }
            st[n] = start;
            ln[n] = i - start;
            n++;
            start = i + 1;
        }
    }
    if (start < len && n < cap) {
        st[n] = start;
        ln[n] = len - start;
        n++;
    }
    return n;
}

static void ref_wc(const char *b, size_t len, unsigned long *lines, unsigned long *words,
                   unsigned long *bytes)
{
    size_t i;
    int in_word = 0;

    *lines = 0;
    *words = 0;
    *bytes = (unsigned long)len;
    for (i = 0; i < len; i++) {
        if (b[i] == '\n') {
            (*lines)++;
        }
        if (ref_blank((unsigned char)b[i])) {
            in_word = 0;
        } else if (!in_word) {
            in_word = 1;
            (*words)++;
        }
    }
}

/* The -n key: leading blanks, an optional sign, digits, an optional fraction. */
typedef struct {
    int neg;
    const char *ip;
    size_t il;
    const char *fp;
    size_t fl;
    int has;
} ref_key_t;

static void ref_key(const char *s, size_t len, ref_key_t *k)
{
    size_t i = 0;

    k->neg = 0;
    k->ip = s;
    k->il = 0;
    k->fp = s;
    k->fl = 0;
    k->has = 0;
    while (i < len && ref_blank((unsigned char)s[i])) {
        i++;
    }
    if (i < len && (s[i] == '-' || s[i] == '+')) {
        k->neg = (s[i] == '-');
        i++;
    }
    k->ip = s + i;
    while (i < len && s[i] >= '0' && s[i] <= '9') {
        i++;
    }
    k->il = (size_t)((s + i) - k->ip);
    if (i < len && s[i] == '.') {
        size_t j = i + 1;

        while (j < len && s[j] >= '0' && s[j] <= '9') {
            j++;
        }
        if (j > i + 1) {
            k->fp = s + i + 1;
            k->fl = j - (i + 1);
        }
    }
    k->has = (k->il != 0 || k->fl != 0);
}

static int ref_too_big(const ref_key_t *k)
{
    return k->il > SH_TEXTOPS_DIGITS_MAX || k->fl > SH_TEXTOPS_DIGITS_MAX;
}

static int ref_bytes(const char *a, const char *b, size_t n)
{
    size_t i;

    for (i = 0; i < n; i++) {
        unsigned char x = (unsigned char)a[i];
        unsigned char y = (unsigned char)b[i];

        if (x != y) {
            return x < y ? -1 : 1;
        }
    }
    return 0;
}

/* Sign, then the integer magnitude with leading zeros stripped (longer is larger, equal
 * length compares the digit strings), then the fraction digit strings. */
static int ref_num_cmp(const ref_key_t *a, const ref_key_t *b)
{
    int av = a->has ? (a->neg ? -1 : 1) : 0;
    int bv = b->has ? (b->neg ? -1 : 1) : 0;
    const char *ai = a->ip;
    const char *bi = b->ip;
    size_t al = a->il;
    size_t bl = b->il;
    size_t n;
    int r = 0;

    if (av != bv) {
        return av < bv ? -1 : 1;
    }
    while (al > 0 && ai[0] == '0') {
        ai++;
        al--;
    }
    while (bl > 0 && bi[0] == '0') {
        bi++;
        bl--;
    }
    if (al != bl) {
        r = al < bl ? -1 : 1;
    } else {
        r = ref_bytes(ai, bi, al);
    }
    if (r == 0) {
        /* Only a tie in the integer part reaches the fraction, and trailing fraction zeros
         * carry no value, so 1, 1.0 and 1.00 share a key. */
        const char *af = a->fp;
        const char *bf = b->fp;
        size_t al = a->fl;
        size_t bl = b->fl;

        while (al > 0 && af[al - 1] == '0') {
            al--;
        }
        while (bl > 0 && bf[bl - 1] == '0') {
            bl--;
        }
        n = al < bl ? al : bl;
        r = ref_bytes(af, bf, n);
        if (r == 0 && al != bl) {
            r = al < bl ? -1 : 1;
        }
    }
    return av < 0 ? -r : r;
}

/* -u compares keys without the byte tie-break, so with -n the lines 1, 01 and 1.0 count
 * as one line. */
static int ref_key_eq(const char *b, size_t la, size_t sa, size_t lb, size_t sb,
                      int numeric)
{
    ref_key_t ka, kb;

    if (!numeric) {
        return la == lb && (la == 0 || memcmp(b + sa, b + sb, la) == 0);
    }
    ref_key(b + sa, la, &ka);
    ref_key(b + sb, lb, &kb);
    return ref_num_cmp(&ka, &kb) == 0;
}

static int ref_cmp(const char *b, size_t la, size_t sa, size_t lb, size_t sb,
                   int numeric)
{
    ref_key_t ka, kb;
    int r;

    if (numeric) {
        ref_key(b + sa, la, &ka);
        ref_key(b + sb, lb, &kb);
        if (ref_too_big(&ka) || ref_too_big(&kb)) {
            return SH_TEXTOPS_BAD_KEY;
        }
        r = ref_num_cmp(&ka, &kb);
        if (r != 0) {
            return r;
        }
    }
    r = ref_bytes(b + sa, b + sb, la < lb ? la : lb);
    if (r != 0) {
        return r;
    }
    if (la == lb) {
        return 0;
    }
    return la < lb ? -1 : 1;
}

/* Plain sort orders whole lines, -r reverses the comparison and -u drops later lines whose
 * key equals an earlier one. The insertion sort is stable, so equals keep input order. */
static size_t ref_sort(const char *b, size_t len, int numeric, int reverse, int uniq,
                       char *out, size_t cap)
{
    size_t st[REF_MAX];
    size_t ln[REF_MAX];
    size_t ord[REF_MAX];
    size_t n = ref_split(b, len, st, ln, REF_MAX);
    size_t used = 0;
    size_t i, j;

    if (n == REF_MAX && len > 0) {
        return (size_t)-1;
    }
    for (i = 0; i < n; i++) {
        ord[i] = i;
    }
    for (i = 1; i < n; i++) {
        size_t keep = ord[i];

        j = i;
        while (j > 0) {
            int r = ref_cmp(b, ln[ord[j - 1]], st[ord[j - 1]], ln[keep], st[keep],
                            numeric);
            if (reverse) {
                r = -r;
            }
            if (r <= 0) {
                break;
            }
            ord[j] = ord[j - 1];
            j--;
        }
        ord[j] = keep;
    }
    /* -u compares keys without the byte tie-break, on the already sorted list, and keeps one
     * line per maximal run of equal keys. The line kept from a run is the run's earliest in
     * the input, which is what makes 1, 01 and 1.0 one line whose survivor is whichever of
     * them came first. Runs are contiguous here because equal keys sort together. */
    if (uniq) {
        size_t w = 0;
        size_t r = 0;

        while (r < n) {
            size_t j = r + 1;
            size_t first = ord[r];

            while (j < n &&
                   ref_key_eq(b, ln[ord[r]], st[ord[r]], ln[ord[j]], st[ord[j]], numeric)) {
                if (ord[j] < first) {
                    first = ord[j];
                }
                j++;
            }
            ord[w++] = first; /* one representative for the whole run */
            r = j;
        }
        n = w;
    }
    for (i = 0; i < n; i++) {
        if (used + ln[ord[i]] + 1 > cap) {
            return used;
        }
        memcpy(out + used, b + st[ord[i]], ln[ord[i]]);
        used += ln[ord[i]];
        out[used++] = '\n';
    }
    return used;
}

/* uniq collapses runs of adjacent byte-identical lines; -c prefixes the count, -d keeps
 * runs of two or more, -u runs of one, and -d with -u keeps nothing. */
static size_t ref_uniq(const char *b, size_t len, int show_count, int dups, int single,
                       char *out, size_t cap)
{
    size_t st[REF_MAX];
    size_t ln[REF_MAX];
    size_t n = ref_split(b, len, st, ln, REF_MAX);
    size_t used = 0;
    size_t i = 0;

    if (n == REF_MAX && len > 0) {
        return (size_t)-1;
    }
    while (i < n) {
        size_t j = i + 1;
        unsigned long count = 1;

        while (j < n && ln[j] == ln[i] && memcmp(b + st[j], b + st[i], ln[i]) == 0) {
            count++;
            j++;
        }
        if ((!dups || count > 1) && (!single || count == 1)) {
            char head[16];
            int hd = 0;

            if (show_count) {
                hd = snprintf(head, sizeof(head), "%7lu ", count);
                if (hd < 0 || used + (size_t)hd + ln[i] + 1 > cap) {
                    return used;
                }
                memcpy(out + used, head, (size_t)hd);
                used += (size_t)hd;
            }
            if (used + ln[i] + 1 > cap) {
                return used;
            }
            memcpy(out + used, b + st[i], ln[i]);
            used += ln[i];
            out[used++] = '\n';
        }
        i = j;
    }
    return used;
}

/* --- running the tools through the in-memory source and sink --- */

static int run_wc(const char *in, size_t len, sh_wc_t *wc)
{
    mem_src_t src;

    src_init(&src, in, len);
    return sh_wc(mem_fill, &src, wc);
}

static int run_sort(const char *in, size_t len, int numeric, int reverse, int uniq,
                    char *out, size_t cap, sink_t *sink)
{
    mem_src_t src;

    src_init(&src, in, len);
    sink_init(sink, out, cap);
    return sh_sort(mem_fill, &src, len, numeric, reverse, uniq, sink_line, sink);
}

static int run_uniq(const char *in, size_t len, int show_count, int dups, int single,
                    char *out, size_t cap, sink_t *sink)
{
    mem_src_t src;

    src_init(&src, in, len);
    sink_init(sink, out, cap);
    sink->with_count = show_count;
    return sh_uniq(mem_fill, &src, len, show_count, dups, single, sink_line, sink);
}

static const char *phase(void)
{
    const char *p = getenv("TEXTOPS_PHASE");

    return p != NULL ? p : "all";
}

/* --- the worked examples from the specification --- */

static void worked_examples(void)
{
    sink_t gl;
    sh_wc_t wc;

    memset(got_buf, 0, sizeof(got_buf));

    memset(&wc, 0, sizeof(wc));
    wc.want_lines = wc.want_words = wc.want_bytes = 1;
    CHECK(run_wc("a b\nc\n", 6, &wc) == 0);
    CHECK(wc.lines == 2 && wc.words == 3 && wc.bytes == 6);
    CHECK(run_wc("", 0, &wc) == 0);
    CHECK(wc.lines == 0 && wc.words == 0 && wc.bytes == 0);
    CHECK(run_wc("a", 1, &wc) == 0);
    CHECK(wc.lines == 0 && wc.words == 1 && wc.bytes == 1);

    CHECK(run_sort("b\na\nb\n", 6, 0, 0, 0, got_buf, OUT_CAP, &gl) == 0);
    CHECK(strcmp(got_buf, "a\nb\nb\n") == 0);
    CHECK(run_sort("b\na\nb\n", 6, 0, 1, 0, got_buf, OUT_CAP, &gl) == 0);
    CHECK(strcmp(got_buf, "b\nb\na\n") == 0);
    CHECK(run_sort("1\n01\n", 5, 1, 0, 0, got_buf, OUT_CAP, &gl) == 0);
    CHECK(strcmp(got_buf, "01\n1\n") == 0);
    CHECK(run_sort("1.10\n1.9\n", 8, 1, 0, 0, got_buf, OUT_CAP, &gl) == 0);
    CHECK(strcmp(got_buf, "1.10\n1.9\n") == 0);
    CHECK(run_sort("10\n9\n-1\nx\n1.5\n", 14, 1, 0, 0, got_buf, OUT_CAP, &gl) == 0);
    CHECK(strcmp(got_buf, "-1\nx\n1.5\n9\n10\n") == 0);
    CHECK(run_sort("b\na\nb\n", 6, 0, 0, 1, got_buf, OUT_CAP, &gl) == 0);
    CHECK(strcmp(got_buf, "a\nb\n") == 0);
    CHECK(run_sort("1\n01\n1.0\n", 9, 1, 0, 1, got_buf, OUT_CAP, &gl) == 0);
    CHECK(strcmp(got_buf, "1\n") == 0);

    CHECK(run_uniq("a\na\nb\na\n", 8, 0, 0, 0, got_buf, OUT_CAP, &gl) == 0);
    CHECK(strcmp(got_buf, "a\nb\na\n") == 0);
    CHECK(run_uniq("a\na\nb\na\n", 8, 1, 0, 0, got_buf, OUT_CAP, &gl) == 0);
    CHECK(strcmp(got_buf, "      2 a\n      1 b\n      1 a\n") == 0);
    CHECK(run_uniq("a\na\nb\na\n", 8, 0, 1, 0, got_buf, OUT_CAP, &gl) == 0);
    CHECK(strcmp(got_buf, "a\n") == 0);
    CHECK(run_uniq("a\na\nb\na\n", 8, 0, 0, 1, got_buf, OUT_CAP, &gl) == 0);
    CHECK(strcmp(got_buf, "b\na\n") == 0);
    CHECK(run_uniq("a\na\nb\na\n", 8, 1, 1, 0, got_buf, OUT_CAP, &gl) == 0);
    CHECK(strcmp(got_buf, "      2 a\n") == 0);
    CHECK(run_uniq("a\na\nb\na\n", 8, 1, 0, 1, got_buf, OUT_CAP, &gl) == 0);
    CHECK(strcmp(got_buf, "      1 b\n      1 a\n") == 0);
    CHECK(run_uniq("a\na\nb\na\n", 8, 0, 1, 1, got_buf, OUT_CAP, &gl) == 0);
    CHECK(strcmp(got_buf, "") == 0);
}

/* --- sh_cmp is the whole comparison, key plus byte tie-break --- */

static void test_cmp(void)
{
    CHECK(sh_cmp("a", 1, "b", 1, 0) < 0);
    CHECK(sh_cmp("b", 1, "a", 1, 0) > 0);
    CHECK(sh_cmp("a", 1, "a", 1, 0) == 0);
    CHECK(sh_cmp("", 0, "a", 1, 0) < 0);     /* a proper prefix sorts first */
    CHECK(sh_cmp("a", 1, "a ", 2, 0) < 0);
    CHECK(sh_cmp("1", 1, "01", 2, 0) > 0);   /* bytes only: '1' is past '0' */
    CHECK(sh_cmp("9", 1, "10", 2, 1) < 0);   /* numbers only: nine is under ten */
    CHECK(sh_cmp("01", 2, "1", 1, 1) < 0);   /* equal keys, so the bytes decide */
    CHECK(sh_cmp("-1", 2, "1", 1, 1) < 0);
    CHECK(sh_cmp("-9", 2, "-10", 3, 1) > 0);
    CHECK(sh_cmp("-0010", 5, "-9", 2, 1) < 0);
    CHECK(ref_key_eq("1\n01", 1, 0, 2, 2, 1));
    CHECK(ref_key_eq("1\n1.0", 1, 0, 3, 2, 1));
    CHECK(ref_key_eq("x\n0", 1, 0, 1, 1, 1));
    CHECK(!ref_key_eq("x\ny", 1, 0, 1, 2, 0));
    CHECK(!ref_key_eq("a\na ", 1, 0, 2, 2, 0));
}

/* --- wc columns: every count is kept even when one was asked for --- */

static void test_wccounts(void)
{
    mem_src_t src;
    sh_wc_t wc;

    src_init(&src, "  a  b \n\n c\n", 12);
    memset(&wc, 0, sizeof(wc));
    wc.want_lines = wc.want_words = wc.want_bytes = 1;
    CHECK(sh_wc(mem_fill, &src, &wc) == 0);
    CHECK(wc.lines == 3); /* three newline bytes, which is what a line is counted by */
    CHECK(wc.words == 3);
    CHECK(wc.bytes == 12);

    /* Only bytes asked for: the other two still come back, because the row is built from
     * them by the caller and a half-count row is worse than a refusal. */
    src_init(&src, "ab\ncd", 5);
    memset(&wc, 0, sizeof(wc));
    wc.want_bytes = 1;
    CHECK(sh_wc(mem_fill, &src, &wc) == 0);
    CHECK(wc.bytes == 5);

    src_init(&src, "", 0);
    memset(&wc, 0, sizeof(wc));
    wc.want_lines = wc.want_words = wc.want_bytes = 1;
    CHECK(sh_wc(mem_fill, &src, &wc) == 0);
    CHECK(wc.lines == 0 && wc.words == 0 && wc.bytes == 0);
}

/* --- the bounds, each named by the errno that says which one --- */

static void test_bounds(void)
{
    mem_src_t src;
    sink_t s;
    sh_wc_t wc;
    char *huge = malloc(SH_TEXTOPS_BYTES_MAX + 64);
    size_t i;

    CHECK(huge != NULL);
    if (huge == NULL) {
        return;
    }
    memset(huge, 'a', SH_TEXTOPS_BYTES_MAX + 64);

    /* Past the byte bound a caller that asks for the truth is refused rather than told a
     * number it cannot check. */
    src_init(&src, huge, SH_TEXTOPS_BYTES_MAX + 16);
    memset(&wc, 0, sizeof(wc));
    wc.want_bytes = 1;
    CHECK(sh_wc(mem_fill, &src, &wc) == -1);
    CHECK(errno == EFBIG);

    /* A caller that says the input is small but feeds more is refused rather than allowed
     * past the bound it was promised. Here every byte is one line, so the line bound is the
     * one met first and the verdict is the one that names it. */
    src_init(&src, huge, SH_TEXTOPS_BYTES_MAX + 16);
    sink_init(&s, got_buf, OUT_CAP);
    CHECK(sh_sort(mem_fill, &src, 16, 0, 0, 0, sink_line, &s) == -1);
    CHECK(errno == E2BIG || errno == EFBIG);

    /* A lied-about size with short lines is the byte bound instead. */
    {
        char *many = malloc(4096);

        CHECK(many != NULL);
        if (many != NULL) {
            for (i = 0; i < 4096; i++) {
                many[i] = (i % 2) ? '\n' : 'a';
            }
            src_init(&src, many, 4096);
            sink_init(&s, got_buf, OUT_CAP);
            CHECK(sh_sort(mem_fill, &src, 8, 0, 0, 0, sink_line, &s) == -1);
            CHECK(errno == EFBIG);
            free(many);
        }
    }
    free(huge);

    /* A line longer than the bound is the line bound, from all three tools. */
    {
        char *long_line = malloc(SH_TEXTOPS_LINE_MAX + 4);

        CHECK(long_line != NULL);
        if (long_line != NULL) {
            memset(long_line, 'a', SH_TEXTOPS_LINE_MAX + 1);
            long_line[SH_TEXTOPS_LINE_MAX + 1] = '\n';
            src_init(&src, long_line, SH_TEXTOPS_LINE_MAX + 2);
            sink_init(&s, got_buf, OUT_CAP);
            CHECK(sh_sort(mem_fill, &src, SH_TEXTOPS_LINE_MAX + 2, 0, 0, 0, sink_line,
                          &s) == -1);
            CHECK(errno == E2BIG);

            src_init(&src, long_line, SH_TEXTOPS_LINE_MAX + 2);
            sink_init(&s, got_buf, OUT_CAP);
            CHECK(sh_uniq(mem_fill, &src, SH_TEXTOPS_LINE_MAX + 2, 0, 0, 0, sink_line,
                          &s) == -1);
            CHECK(errno == E2BIG);

            /* wc keeps no line, so a single huge line is not its bound: the counts are
             * still exact, and only the total byte bound can refuse. */
            src_init(&src, long_line, SH_TEXTOPS_LINE_MAX + 2);
            memset(&wc, 0, sizeof(wc));
            wc.want_lines = 1;
            CHECK(sh_wc(mem_fill, &src, &wc) == 0);
            CHECK(wc.bytes == SH_TEXTOPS_LINE_MAX + 2);
            free(long_line);
        }
    }
}

/* --- a numeric key too long to represent --- */

static void test_badkey(void)
{
    mem_src_t src;
    sink_t s;
    char *in;
    size_t i;
    /* One digit past the bound, so the key really is unrepresentable. */
    size_t n = SH_TEXTOPS_DIGITS_MAX + 4;

    in = malloc(n + 4);
    CHECK(in != NULL);
    if (in == NULL) {
        return;
    }
    in[0] = 'a';
    in[1] = '\n';
    for (i = 0; i < SH_TEXTOPS_DIGITS_MAX + 1; i++) {
        in[2 + i] = '9';
    }
    in[n - 1] = '\n'; /* the last byte of the input, not one past it */
    src_init(&src, in, n);
    sink_init(&s, got_buf, OUT_CAP);
    CHECK(sh_sort(mem_fill, &src, n, 1, 0, 0, sink_line, &s) == -1);
    CHECK(errno == EILSEQ);

    /* Without -n the same input is an ordinary byte sort. */
    src_init(&src, in, n);
    sink_init(&s, got_buf, OUT_CAP);
    CHECK(sh_sort(mem_fill, &src, n, 0, 0, 0, sink_line, &s) == 0);

    /* And only the -n answer is refused: a non-numeric key never looks at the digits. */
    src_init(&src, in, n);
    sink_init(&s, got_buf, OUT_CAP);
    CHECK(sh_uniq(mem_fill, &src, n, 0, 0, 0, sink_line, &s) == 0);
    free(in);
}

/* --- a source that fails, and a sink that stops --- */

static void test_failures(void)
{
    mem_src_t src;
    sink_t s;
    sh_wc_t wc;

    src_init(&src, "a\nb\n", 4);
    src.fail_at = 2; /* fails past the first line */
    memset(&wc, 0, sizeof(wc));
    wc.want_bytes = 1;
    CHECK(sh_wc(mem_fill, &src, &wc) == -1);
    CHECK(errno == EIO);

    src_init(&src, "a\nb\n", 4);
    src.fail_at = 2;
    sink_init(&s, got_buf, OUT_CAP);
    CHECK(sh_sort(mem_fill, &src, 4, 0, 0, 0, sink_line, &s) == -1);
    CHECK(errno == EIO);

    src_init(&src, "a\nb\n", 4);
    src.fail_at = 2;
    sink_init(&s, got_buf, OUT_CAP);
    CHECK(sh_uniq(mem_fill, &src, 4, 0, 0, 0, sink_line, &s) == -1);
    CHECK(errno == EIO);

    /* A sink that stops ends the tool rather than growing without bound. */
    src_init(&src, "a\nb\nc\n", 6);
    sink_init(&s, got_buf, 2);
    CHECK(sh_sort(mem_fill, &src, 6, 0, 0, 0, sink_line, &s) == -1);

    src_init(&src, "a\nb\nc\n", 6);
    sink_init(&s, got_buf, 2);
    CHECK(sh_uniq(mem_fill, &src, 6, 0, 0, 0, sink_line, &s) == -1);

    src_init(&src, "a\nb\n", 4);
    sink_init(&s, got_buf, OUT_CAP);
    s.refuse = 1;
    CHECK(sh_sort(mem_fill, &src, 4, 0, 0, 0, sink_line, &s) == -1);
}

/* --- every string up to MAX_LEN over ALPHA, against the naive references --- */

static void test_exhaustive(void)
{
    char in[MAX_LEN + 2];
    sink_t s;
    size_t len;
    int slots[MAX_LEN];
    size_t nalpha = strlen(ALPHA);
    const char *env_len = getenv("TEXTOPS_LEN");
    size_t max_len = MAX_LEN;

    if (env_len != NULL) {
        max_len = (size_t)atoi(env_len);
    }
    for (len = 0; len <= max_len; len++) {
        size_t k;

        for (k = 0; k < (size_t)len; k++) {
            slots[k] = 0;
        }
        for (;;) {
            int numeric, reverse, uniq, show_count, dups, single;

            for (k = 0; k < (size_t)len; k++) {
                in[k] = ALPHA[slots[k]];
            }
            in[len] = '\0';
            tick("in", len);

            for (numeric = 0; numeric <= 1; numeric++) {
                for (reverse = 0; reverse <= 1; reverse++) {
                    for (uniq = 0; uniq <= 1; uniq++) {
                        size_t rl;
                        int rr;

                        rl = ref_sort(in, len, numeric, reverse, uniq, ref_buf, OUT_CAP);
                        rr = run_sort(in, len, numeric, reverse, uniq, got_buf, OUT_CAP,
                                      &s);
                        CHECK(rr == 0);
                        if (!(s.len == rl && memcmp(got_buf, ref_buf, rl) == 0)) {
                            printf("FAIL sort n=%d r=%d u=%d len=%zu |", numeric, reverse,
                                   uniq, len);
                            {
                                size_t z;
                                for (z = 0; z < len; z++) printf(" %02x", (unsigned char)in[z]);
                                printf(" | got=");
                                for (z = 0; z < s.len; z++) printf(" %02x", (unsigned char)got_buf[z]);
                                printf(" | ref=");
                                for (z = 0; z < rl; z++) printf(" %02x", (unsigned char)ref_buf[z]);
                                printf("\n");
                            }
                            failures++;
                        }
                    }
                }
            }
            for (show_count = 0; show_count <= 1; show_count++) {
                for (dups = 0; dups <= 1; dups++) {
                    for (single = 0; single <= 1; single++) {
                        size_t rl;
                        int rr;

                        rl = ref_uniq(in, len, show_count, dups, single, ref_buf, OUT_CAP);
                        rr = run_uniq(in, len, show_count, dups, single, got_buf, OUT_CAP,
                                      &s);
                        CHECK(rr == 0);
                        if (!(s.len == rl && memcmp(got_buf, ref_buf, rl) == 0)) {
                            printf("FAIL uniq c=%d d=%d s=%d in=<%s> got=<%s> ref=<%s>\n",
                                   show_count, dups, single, in, got_buf, ref_buf);
                            failures++;
                        }
                    }
                }
            }
            {
                sh_wc_t wc;
                unsigned long l, w, by;

                memset(&wc, 0, sizeof(wc));
                wc.want_lines = wc.want_words = wc.want_bytes = 1;
                CHECK(run_wc(in, len, &wc) == 0);
                ref_wc(in, len, &l, &w, &by);
                CHECK(wc.lines == l && wc.words == w && wc.bytes == by);
            }

            /* count up through the alphabet, least significant slot last */
            {
                int at = (int)len - 1;

                while (at >= 0) {
                    slots[at]++;
                    if (slots[at] < (int)nalpha) {
                        break;
                    }
                    slots[at] = 0;
                    at--;
                }
                if (at < 0) {
                    break;
                }
            }
        }
    }
}

int main(void)
{
    const char *ph = phase();

    if (strcmp(ph, "all") == 0 || strcmp(ph, "worked") == 0) {
        worked_examples();
        printf("worked: %d failures\n", failures);
    }
    if (strcmp(ph, "all") == 0 || strcmp(ph, "cmp") == 0) {
        test_cmp();
        printf("cmp: %d failures\n", failures);
    }
    if (strcmp(ph, "all") == 0 || strcmp(ph, "wccounts") == 0) {
        test_wccounts();
        printf("wccounts: %d failures\n", failures);
    }
    if (strcmp(ph, "all") == 0 || strcmp(ph, "bounds") == 0) {
        test_bounds();
        printf("bounds: %d failures\n", failures);
    }
    if (strcmp(ph, "all") == 0 || strcmp(ph, "badkey") == 0) {
        test_badkey();
        printf("badkey: %d failures\n", failures);
    }
    if (strcmp(ph, "all") == 0 || strcmp(ph, "failures") == 0) {
        test_failures();
        printf("failures: %d failures\n", failures);
    }
    if (strcmp(ph, "all") == 0 || strcmp(ph, "exh") == 0) {
        test_exhaustive();
        printf("exh: %d failures\n", failures);
    }
    return failures != 0 ? 1 : 0;
}
