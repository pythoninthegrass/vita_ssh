/* The counting, ordering and collapsing behind wc, sort and uniq: no I/O, no paths and no
 * Vita APIs, so the host check in scripts/test_sh_textops.c runs the same code the server
 * links. Bounds come back as errno and each builtin reports them in its own voice. */
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

/* One chunk from the source: how many bytes arrived (0 at end of input), or -1 with the
 * fill's errno. */
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
    /* Nothing is kept between chunks, so the only bound that can be passed is the total
     * byte count, and every count stays exact right up to the refusal. */
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
            /* Every column is counted, not only the wanted ones: the field width comes
             * from the byte count of the largest input, which the caller learns last. */
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

/* The -n key of a line: leading blanks, an optional sign, digits, an optional fraction. A
 * line with none of them has no key and counts as zero. */
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

/* Sign first, so a line with no number sits between the negatives and the positives; then
 * the integer magnitude with leading zeros stripped, where longer is larger and equal
 * lengths compare their digit strings; then the fraction digit strings. */
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
        /* Trailing fraction zeros carry no value, so 1, 1.0 and 1.00 share a key; what is
         * compared is the fraction with them gone. */
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

/* Whether two neighbouring lines are one line as far as -u is concerned: the same bytes
 * without -n, the same number with it, and never the byte tie-break. */
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

/* Reads a source one line at a time into one growing accumulator, which is the shape both
 * tools that keep lines need: sort holds every line and uniq holds two. */
typedef struct {
    char *acc;
    size_t len;
    size_t cap;
    size_t total; /* bytes attributed to lines, which is the bound the caller honours */
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

/* The next byte into *c: 1 for a byte, 0 for end of input, or -1 with errno set. A NUL is
 * a byte like any other, so the verdict travels separately from the value, and end of
 * input is known only once a read has come back empty, which is what eof records. */
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
        /* Never ask for more than the chunk holds, or a caller with a small buffer would
         * be written past. */
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

/* The next line without its newline, or -1 with errno E2BIG past the line bound, ENOMEM
 * past the accumulator, or the read's errno. A line ends at a newline or at end of input,
 * so a last line with no newline is still a line and "\n" is one empty line; 0 with *done
 * means the input ended on a newline and no line follows. */
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
            /* End of input gives a line only when bytes arrived during this call: that is
             * the last line, written without a newline. Once the source has been drained,
             * every later call is done, which is what stops the reader repeating it. */
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

/* The comparison sort uses. With -n the key decides and -r applies to the key alone; the
 * byte order is the tie-break, where a proper prefix sorts first. */
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

/* One merge pass: src holds runs of width and dst receives them merged two at a time.
 * Taking the left run whenever the two compare equal is what keeps the sort stable, which
 * is what lets -u keep the first of a set of equal lines in input order. */
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

    /* Everything is held before anything is printed, because the last line can sort first;
     * the line is copied into the blob as it arrives so no second copy is needed. */
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
        /* The caller states the size it believes it has; the bytes actually consumed are
         * checked against both that and the stated maximum, so a source that lies about its
         * length cannot walk past the bound it was promised. */
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

    /* A key with more digits than the bound is refused before any comparison is trusted,
     * so the whole input is scanned for it rather than only the pairs a merge happens to
     * compare: two lines never compared still have to fail the run. */
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

    /* -u is a verdict on the key alone, with no byte tie-break, and it is transitive: all
     * the lines sharing a key form one group no matter where the tie-break put them, and
     * the first of them in input order is the one kept. Grouping by key first, then taking
     * each group's earliest line, is what makes 1, 01 and 1.0 one line while still printing
     * the survivor at the position the key comparison ordered it. */
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
                /* One group per key: every other line with the same key is dropped, and the
                 * line kept is the group's earliest in the input, not whichever the byte
                 * tie-break happened to leave first. The whole group is marked, including
                 * the survivor, so a survivor sitting earlier in the sorted order is not
                 * emitted a second time when the walk reaches it. */
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
                /* A sink that stops is the caller's answer channel closing, which is not
                 * the tool's own failure; ENOMEM says the answer could not be delivered. */
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

/* Whether one collapsed run is printed: -d keeps runs of two or more, -u runs of one, and
 * asking for both keeps nothing. The count is passed out so -c can prefix it. */
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

    /* Only the run in hand is held: a line is compared with the one before it, so a long
     * file costs one line of memory rather than the file. */
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



