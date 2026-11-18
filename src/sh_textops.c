/* Counting, ordering and collapsing for wc, sort, uniq; notes in docs/shell-text-tools.md. */
#include "sh_textops.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* Bytes taken from the source at a time, small enough to sit on a thread's stack. */
#define FILL_CHUNK 4096

static int is_blank(int c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r';
}

/* One chunk from the source: bytes arrived (0 at end of input), or -1 with the fill's errno. */
static ssize_t pull(sh_textops_fill_fn fill, void *ctx, char *p, size_t want)
{
    ssize_t n;

    if (want == 0) {
        return 0;
    }
    n = fill(ctx, p, want);
    return n < 0 ? -1 : n;
}

int sh_wc(sh_textops_fill_fn fill, void *ctx, sh_wc_t *wc)
{
    char *chunk;
    int in_word = 0;

    wc->lines = 0;
    wc->words = 0;
    wc->bytes = 0;
    chunk = malloc(FILL_CHUNK);
    if (chunk == NULL) {
        errno = ENOMEM;
        return -1;
    }
    /* Nothing is kept between chunks, so only the total byte count can pass the bound. */
    for (;;) {
        ssize_t n = pull(fill, ctx, chunk, FILL_CHUNK);
        ssize_t i;

        if (n < 0) {
            free(chunk);
            return -1;
        }
        if (n == 0) {
            break;
        }
        if (wc->bytes + (unsigned long)n > SH_TEXTOPS_BYTES_MAX) {
            free(chunk);
            errno = EFBIG;
            return -1;
        }
        for (i = 0; i < n; i++) {
            unsigned char c = (unsigned char)chunk[i];

            if (c == '\n') {
                wc->lines++;
            }
            /* Every column is counted: field width needs the largest byte count, known last. */
            if (is_blank(c)) {
                in_word = 0;
            } else if (!in_word) {
                in_word = 1;
                wc->words++;
            }
        }
        wc->bytes += (unsigned long)n;
    }
    free(chunk);
    return 0;
}

/* The -n key of a line: blanks, optional sign, digits, optional fraction; none means zero. */
typedef struct {
    int neg;
    const char *ip;
    size_t il;
    const char *fp;
    size_t fl;
    int has;
} numkey_t;

static void parse_key(const char *s, size_t len, numkey_t *k)
{
    size_t i = 0;

    k->neg = 0;
    k->ip = s;
    k->il = 0;
    k->fp = s;
    k->fl = 0;
    k->has = 0;
    while (i < len && is_blank((unsigned char)s[i])) {
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
    /* A trailing dot is not a number on its own: "1." has the key 1 and "x." none. */
    k->has = (k->il != 0 || k->fl != 0);
}

static int digits_over(const numkey_t *k)
{
    return k->il > SH_TEXTOPS_DIGITS_MAX || k->fl > SH_TEXTOPS_DIGITS_MAX;
}

/* Byte order over unsigned bytes, which is what C locale ordering means here. */
static int byte_cmp(const char *a, const char *b, size_t n)
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

/* Order by sign, then integer magnitude without leading zeros, then fraction digits. */
static int key_cmp(const numkey_t *a, const numkey_t *b)
{
    int av = a->has ? (a->neg ? -1 : 1) : 0;
    int bv = b->has ? (b->neg ? -1 : 1) : 0;
    const char *ai = a->ip;
    const char *bi = b->ip;
    size_t al = a->il;
    size_t bl = b->il;
    size_t common;
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
        r = byte_cmp(ai, bi, al);
    }
    if (r == 0) {
        /* Trailing fraction zeros carry no value, so 1, 1.0 and 1.00 share a key. */
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
        common = al < bl ? al : bl;
        r = byte_cmp(af, bf, common);
        if (r == 0 && al != bl) {
            r = al < bl ? -1 : 1;
        }
    }
    return av < 0 ? -r : r;
}

int sh_cmp(const char *a, size_t al, const char *b, size_t bl, int numeric)
{
    numkey_t ka, kb;
    int r;

    if (numeric) {
        parse_key(a, al, &ka);
        parse_key(b, bl, &kb);
        if (digits_over(&ka) || digits_over(&kb)) {
            return SH_TEXTOPS_BAD_KEY;
        }
        r = key_cmp(&ka, &kb);
        if (r != 0) {
            return r;
        }
    }
    r = byte_cmp(a, b, al < bl ? al : bl);
    if (r != 0) {
        return r;
    }
    /* Equal up to the shorter line: a proper prefix sorts first. */
    if (al == bl) {
        return 0;
    }
    return al < bl ? -1 : 1;
}

/* Whether neighboring lines are one line for -u: same bytes, or same number with -n. */
static int key_equal(const char *a, size_t al, const char *b, size_t bl, int numeric)
{
    numkey_t ka, kb;

    if (!numeric) {
        return al == bl && (al == 0 || memcmp(a, b, al) == 0);
    }
    parse_key(a, al, &ka);
    parse_key(b, bl, &kb);
    if (digits_over(&ka) || digits_over(&kb)) {
        return 0;
    }
    return key_cmp(&ka, &kb) == 0;
}

/* Reads a source line by line into one growing accumulator, as sort and uniq both need. */
typedef struct {
    char *acc;
    size_t len;
    size_t cap;
    size_t total; /* bytes attributed to lines, which is the bound the caller honors */
    size_t chunk_cap; /* how many bytes chunk can hold, which is also the most asked for */
    size_t seen;    /* every byte taken from the source */
    size_t lines;   /* lines handed back so far, which is what the line bound counts */
    char *chunk;
    size_t chunk_len;
    size_t chunk_off;
    int eof;
} reader_t;

static void reader_init(reader_t *r, char *acc, size_t cap, char *chunk,
                        size_t chunk_cap)
{
    r->acc = acc;
    r->len = 0;
    r->cap = cap;
    r->total = 0;
    r->seen = 0;
    r->lines = 0;
    r->chunk = chunk;
    r->chunk_cap = chunk_cap;
    r->chunk_len = 0;
    r->chunk_off = 0;
    r->eof = 0;
}

/* The next byte into *c: 1 for a byte, 0 at end of input, -1 with errno; eof records the end. */
static int reader_byte(reader_t *r, sh_textops_fill_fn fill, void *ctx, int *c)
{
    for (;;) {
        ssize_t n;

        if (r->chunk_off < r->chunk_len) {
            *c = (unsigned char)r->chunk[r->chunk_off++];
            return 1;
        }
        if (r->eof) {
            return 0;
        }
        /* Never ask for more than the chunk holds, or a small caller buffer would be overrun. */
        n = pull(fill, ctx, r->chunk, r->chunk_cap);
        if (n < 0) {
            return -1;
        }
        r->chunk_len = (size_t)n;
        r->chunk_off = 0;
        r->seen += (size_t)n;
        if (n == 0) {
            r->eof = 1;
            return 0;
        }
    }
}

/* The next line without its newline, 0 with *done at end of input, or -1 with errno. */
static ssize_t reader_line(reader_t *r, sh_textops_fill_fn fill, void *ctx, int *done)
{
    int saw_byte = 0;

    r->len = 0;
    *done = 0;
    for (;;) {
        int c = 0;
        int got = reader_byte(r, fill, ctx, &c);

        if (got < 0) {
            return -1;
        }
        if (got == 0) {
            /* End of input yields the last unterminated line once; later calls report done. */
            if (!saw_byte) {
                *done = 1;
                return 0;
            }
            r->total += r->len;
            r->lines++;
            return (ssize_t)r->len;
        }
        saw_byte = 1;
        if (c == '\n') {
            r->total += r->len + 1;
            r->lines++;
            return (ssize_t)r->len;
        }
        if (r->len >= SH_TEXTOPS_LINE_MAX) {
            errno = E2BIG;
            return -1;
        }
        if (r->len >= r->cap) {
            errno = ENOMEM;
            return -1;
        }
        r->acc[r->len++] = (char)c;
    }
}

/* Sort's record of one line: where its bytes live in the blob, and the key they carry. */
typedef struct {
    size_t off;
    size_t len;
    size_t seq; /* position in the input, so -u can keep the earliest line of a key group */
    numkey_t key;
} item_t;

/* The sort comparison: -n key first with -r on the key alone, byte order as tie-break. */
static int cmp_items(const item_t *a, const item_t *b, const char *blob, int numeric,
                     int reverse, int *bad)
{
    int r;

    if (numeric) {
        if (digits_over(&a->key) || digits_over(&b->key)) {
            *bad = 1;
            return 0;
        }
        r = key_cmp(&a->key, &b->key);
        if (r != 0) {
            return reverse ? -r : r;
        }
    }
    r = byte_cmp(blob + a->off, blob + b->off, a->len < b->len ? a->len : b->len);
    if (r != 0) {
        return reverse ? -r : r;
    }
    if (a->len == b->len) {
        return 0;
    }
    r = a->len < b->len ? -1 : 1;
    return reverse ? -r : r;
}

/* One merge pass of runs of width; taking the left run on ties keeps the sort stable. */
static void merge_pass(const item_t *src, size_t n, size_t width, const char *blob,
                       int numeric, int reverse, int *bad, item_t *dst)
{
    size_t lo;

    for (lo = 0; lo < n && !*bad; lo += 2 * width) {
        size_t mid = lo + width > n ? n : lo + width;
        size_t hi = lo + 2 * width > n ? n : lo + 2 * width;
        size_t i = lo;
        size_t j = mid;
        size_t k = lo;

        while (i < mid && j < hi) {
            int r = cmp_items(&src[i], &src[j], blob, numeric, reverse, bad);

            if (r <= 0) {
                dst[k++] = src[i++];
            } else {
                dst[k++] = src[j++];
            }
        }
        while (i < mid) {
            dst[k++] = src[i++];
        }
        while (j < hi) {
            dst[k++] = src[j++];
        }
    }
}

int sh_sort(sh_textops_fill_fn fill, void *ctx, size_t size, int numeric, int reverse,
            int uniq, sh_textops_line_fn emit, void *emit_ctx)
{
    item_t *items = NULL;
    item_t *merge = NULL;
    char *blob = NULL;
    char *scratch = NULL;
    char *run = NULL;
    reader_t r;
    size_t cap_items = 0;
    size_t used = 0;
    size_t n = 0;
    size_t width;
    int bad = 0;
    int status = -1;

    if (size > SH_TEXTOPS_BYTES_MAX) {
        errno = EFBIG;
        return -1;
    }
    blob = malloc(size != 0 ? size : 1);
    scratch = malloc(FILL_CHUNK);
    run = malloc(SH_TEXTOPS_LINE_MAX);
    items = malloc(sizeof(item_t));
    merge = malloc(sizeof(item_t));
    if (blob == NULL || scratch == NULL || run == NULL || items == NULL ||
        merge == NULL) {
        status = -1;
        errno = ENOMEM;
        goto out;
    }
    cap_items = 1;
    reader_init(&r, run, SH_TEXTOPS_LINE_MAX, scratch, FILL_CHUNK);

    /* Everything is held before printing because the last line can sort first. */
    for (;;) {
        int done = 0;
        ssize_t len = reader_line(&r, fill, ctx, &done);

        /* done, not len, is the end: an empty line arrives as 0 and is still a line. */
        if (len < 0) {
            goto out;
        }
        if (done) {
            break;
        }
        /* Consumed bytes are checked against the stated size and the maximum, not the source. */
        if (r.total > size || r.total > SH_TEXTOPS_BYTES_MAX) {
            errno = EFBIG;
            goto out;
        }
        if (r.lines > SH_TEXTOPS_LINES_MAX) {
            errno = EFBIG;
            goto out;
        }
        if (n == cap_items) {
            size_t want = cap_items * 2;
            item_t *grown = realloc(items, want * sizeof(item_t));
            item_t *more;

            if (grown == NULL) {
                errno = ENOMEM;
                goto out;
            }
            items = grown;
            more = realloc(merge, want * sizeof(item_t));
            if (more == NULL) {
                errno = ENOMEM;
                goto out;
            }
            merge = more;
            cap_items = want;
        }
        memcpy(blob + used, run, (size_t)len);
        items[n].off = used;
        items[n].len = (size_t)len;
        items[n].seq = n;
        parse_key(blob + used, (size_t)len, &items[n].key);
        used += (size_t)len; /* blob bytes; r.total is what the byte bound compares */
        n++;
    }

    /* Scan every key for an over-long digit run, since a merge may never compare some pairs. */
    if (numeric) {
        for (size_t i = 0; i < n; i++) {
            if (digits_over(&items[i].key)) {
                bad = 1;
                break;
            }
        }
    }

    for (width = 1; width < n && !bad; width *= 2) {
        merge_pass(items, n, width, blob, numeric, reverse, &bad, merge);
        {
            item_t *swap = items;

            items = merge;
            merge = swap;
        }
    }
    if (bad) {
        errno = EILSEQ;
        goto out;
    }

    /* -u compares keys only: group by key, keep each group's earliest line in input order. */
    {
        char *seen = NULL; /* one flag per line, cleared as groups are consumed */
        size_t i;

        if (uniq) {
            seen = calloc(n != 0 ? n : 1, 1);
            if (seen == NULL) {
                errno = ENOMEM;
                goto out;
            }
        }
        for (i = 0; i < n; i++) {
            size_t j;
            int rc;

            if (uniq && seen[i]) {
                continue;
            }
            if (uniq) {
                /* Drop all but the key group's earliest line; mark the survivor too. */
                size_t first = i;

                seen[i] = 1;
                for (j = i + 1; j < n; j++) {
                    if (!seen[j] &&
                        key_equal(blob + items[i].off, items[i].len, blob + items[j].off,
                                  items[j].len, numeric)) {
                        seen[j] = 1;
                        if (items[j].seq < items[first].seq) {
                            first = j;
                        }
                    }
                }
                rc = emit(emit_ctx, blob + items[first].off, items[first].len, 1);
            } else {
                rc = emit(emit_ctx, blob + items[i].off, items[i].len, 1);
            }
            if (rc != 0) {
                free(seen);
                /* A stopping sink is not the tool's failure; ENOMEM means the answer was lost. */
                errno = ENOMEM;
                status = -1;
                goto out;
            }
        }
        free(seen);
    }
    status = 0;

out:
    free(items);
    free(merge);
    free(blob);
    free(scratch);
    free(run);
    return status;
}

/* Whether a run is printed: -d keeps 2 or more, -u exactly 1, both nothing; count out for -c. */
static int run_shown(int dups_only, int singles_only, unsigned long count)
{
    if (dups_only && singles_only) {
        return 0;
    }
    if (dups_only) {
        return count >= 2;
    }
    if (singles_only) {
        return count == 1;
    }
    return 1;
}

/* Hands a finished run to the caller, and says whether the tool may carry on. */
static int emit_run(sh_textops_line_fn emit, void *emit_ctx, int dups_only,
                    int singles_only, const char *line, size_t len, unsigned long count)
{
    if (!run_shown(dups_only, singles_only, count)) {
        return 1;
    }
    return emit(emit_ctx, line, len, count) == 0;
}

int sh_uniq(sh_textops_fill_fn fill, void *ctx, size_t size, int show_count,
            int dups_only, int singles_only, sh_textops_line_fn emit, void *emit_ctx)
{
    char *scratch = malloc(FILL_CHUNK);
    char *cur = malloc(SH_TEXTOPS_LINE_MAX);
    char *nxt = malloc(SH_TEXTOPS_LINE_MAX);
    reader_t r;
    size_t total = 0;
    size_t runs = 0;
    size_t cur_len = 0;
    unsigned long count = 0;
    int have = 0;
    int status = -1;

    (void)show_count; /* the prefix is the caller's, and so is the field width */
    if (size > SH_TEXTOPS_BYTES_MAX) {
        free(scratch);
        free(cur);
        free(nxt);
        errno = EFBIG;
        return -1;
    }
    if (scratch == NULL || cur == NULL || nxt == NULL) {
        free(scratch);
        free(cur);
        free(nxt);
        errno = ENOMEM;
        return -1;
    }
    reader_init(&r, nxt, SH_TEXTOPS_LINE_MAX, scratch, FILL_CHUNK);

    /* Only the run in hand is held, so a long file costs one line of memory. */
    for (;;) {
        int done = 0;
        ssize_t len = reader_line(&r, fill, ctx, &done);

        if (len < 0) {
            goto out;
        }
        if (done) {
            break;
        }
        total = r.total;
        if (total > size || total > SH_TEXTOPS_BYTES_MAX) {
            errno = EFBIG;
            goto out;
        }
        if (!have || cur_len != r.len || memcmp(cur, nxt, r.len) != 0) {
            if (have && !emit_run(emit, emit_ctx, dups_only, singles_only, cur, cur_len,
                                  count)) {
                errno = ENOMEM;
                status = -1;
                goto out;
            }
            memcpy(cur, nxt, r.len);
            cur_len = r.len;
            count = 1;
            have = 1;
            runs++;
        } else {
            count++;
        }
    }
    if (have && !emit_run(emit, emit_ctx, dups_only, singles_only, cur, cur_len, count)) {
        errno = ENOMEM;
        status = -1;
        goto out;
    }
    status = 0;

out:
    free(scratch);
    free(cur);
    free(nxt);
    (void)runs;
    return status;
}



