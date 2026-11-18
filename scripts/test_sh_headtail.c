/* Host test for src/sh_headtail.c, driven by test_sh_headtail.py, against a naive reference. */
#include "sh_headtail.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

#define CHECK(cond)                                                                 \
    do {                                                                            \
        if (!(cond)) {                                                              \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                 \
            failures++;                                                             \
        }                                                                           \
    } while (0)

/* A file in memory, behind the same seekable callback the server's file source uses. */
typedef struct {
    const char *p;
    size_t len;
    size_t reads;     /* how many times read_at was called */
    size_t max_once;  /* largest n ever asked for in one call */
} mem_file_t;

static ssize_t mem_read_at(void *vctx, void *p, size_t n, size_t off)
{
    mem_file_t *f = vctx;
    size_t left;

    f->reads++;
    if (n > f->max_once) {
        f->max_once = n;
    }
    if (off >= f->len) {
        return 0;
    }
    left = f->len - off;
    if (n > left) {
        n = left;
    }
    memcpy(p, f->p + off, n);
    return (ssize_t)n;
}

/* Fails every read at or past a set offset to fake a mid-file I/O error; embeds a mem_file_t. */
typedef struct {
    mem_file_t file;
    size_t fail_at;
} sick_file_t;

static ssize_t sick_read_at(void *vctx, void *p, size_t n, size_t off)
{
    sick_file_t *f = vctx;

    if (off >= f->fail_at) {
        errno = EIO;
        return -1;
    }
    return mem_read_at(&f->file, p, n, off);
}

/* Naive reference from the specification: head takes the first want lines, tail the last want. */
static size_t ref_head(const char *b, size_t len, size_t want, char *out)
{
    size_t i;
    size_t lines = 0;

    if (want == 0) {
        return 0;  /* head -n 0 prints nothing */
    }
    for (i = 0; i < len; i++) {
        if (b[i] != '\n') {
            continue;
        }
        lines++;
        if (lines == want) {
            memcpy(out, b, i + 1);  /* through the want'th newline, inclusive */
            return i + 1;
        }
    }
    memcpy(out, b, len);  /* fewer newlines than asked: the file is the answer */
    return len;
}

static size_t ref_tail(const char *b, size_t len, size_t want, char *out)
{
    size_t end = len;
    size_t i;
    size_t count = 0;

    if (want == 0 || len == 0) {
        return 0;  /* tail -n 0 prints nothing */
    }
    if (b[len - 1] == '\n') {
        end = len - 1;  /* that newline ends the last line, it does not open one */
    }
    for (i = end; i > 0; i--) {
        if (b[i - 1] != '\n') {
            continue;
        }
        count++;
        if (count == want) {
            memcpy(out, b + i, len - i);  /* after it, and running to the file's end */
            return len - i;
        }
    }
    memcpy(out, b, len);  /* fewer lines than asked: the file is the answer */
    return len;
}

/* File-scope scratch buffers sized for the widest answer; ASan frames are small, so no stack. */
#define SCRATCH (SH_HT_LINE_MAX + 4 * SH_HT_CHUNK)
static char case_file[SCRATCH];
static char case_got[SCRATCH];
static char case_exp[SCRATCH];

/* One file through head -n and tail -n, each compared with the reference. */
static void check_lines_case(const char *b, size_t len, size_t want)
{
    size_t el;
    ssize_t g;
    size_t start;
    size_t end;
    size_t tstart;

    el = ref_head(b, len, want, case_exp);
    start = 1;
    end = 1;
    g = -12345;
    {
        mem_file_t f = {b, len, 0, 0};

        g = sh_head_lines(mem_read_at, &f, want, case_got, SCRATCH, NULL, NULL, &start, &end);
    }
    CHECK(g == (ssize_t)el);
    CHECK(start == 0);
    CHECK((size_t)(g > 0 ? g : 0) == end);
    if (g > 0) {
        CHECK(memcmp(case_got, case_exp, el) == 0);
    }

    el = ref_tail(b, len, want, case_exp);
    tstart = (size_t)-1;
    g = -12345;
    {
        mem_file_t f = {b, len, 0, 0};

        g = sh_tail_lines(mem_read_at, &f, len, want, case_got, SCRATCH, NULL, NULL, &tstart);
    }
    CHECK(g == (ssize_t)el);
    if (g > 0) {
        CHECK(tstart == len - (size_t)g);
    } else {
        CHECK(tstart == (want == 0 ? len : 0));
    }
    if (g > 0) {
        CHECK(memcmp(case_got, case_exp, el) == 0);
    }
}

/* One file through head -c and tail -c: a byte window is that many bytes, verbatim. */
static void check_bytes_case(const char *b, size_t len, size_t want)
{
    size_t el = (want < len) ? want : len;
    ssize_t g;

    {
        mem_file_t f = {b, len, 0, 0};

        g = sh_head_bytes(mem_read_at, &f, want, case_got, SCRATCH);
    }
    CHECK(g == (ssize_t)el);
    if (g > 0) {
        CHECK(memcmp(case_got, b, el) == 0);
    }
    {
        mem_file_t f = {b, len, 0, 0};

        g = sh_tail_bytes(mem_read_at, &f, len, want, case_got, SCRATCH);
    }
    CHECK(g == (ssize_t)el);
    if (g > 0) {
        CHECK(memcmp(case_got, b + (len - el), el) == 0);
    }
}

/* Every string up to length MAX over {a, b, newline}, for want in 0..ENUM_WANT. */
#define ENUM_MAX 9
#define ENUM_WANT 5

static void test_against_reference(void)
{
    size_t n;
    size_t w;

    for (n = 0; n <= ENUM_MAX; n++) {
        size_t total = 1;
        size_t code;

        for (size_t k = 0; k < n; k++) {
            total *= 3;
        }
        for (code = 0; code < total; code++) {
            size_t x = code;

            for (size_t k = 0; k < n; k++) {
                static const char alpha[3] = {'a', 'b', '\n'};

                case_file[k] = alpha[x % 3];
                x /= 3;
            }
            for (w = 0; w <= ENUM_WANT; w++) {
                check_lines_case(case_file, n, w);
                check_bytes_case(case_file, n, w);
            }
            if (failures > 6) {
                printf("FAIL too many failures, stopping enumeration\n");
                return;
            }
        }
    }
}

/* The worked examples, spelled out so a regression names the case it broke. */
static void test_worked_examples(void)
{
    static const struct {
        const char *text;
        size_t want;
        const char *head;  /* the bytes `head -n want` prints */
        const char *tail;  /* the bytes `tail -n want` prints */
    } cases[] = {
        {"one\ntwo\nthree\n", 2, "one\ntwo\n", "two\nthree\n"},
        {"one\ntwo\nthree", 2, "one\ntwo\n", "two\nthree"},
        {"aa\n\nbaa", 1, "aa\n", "baa"},
        {"aa\n\nbaa", 2, "aa\n\n", "\nbaa"},
        {"aa\n\nbaa", 3, "aa\n\nbaa", "aa\n\nbaa"},
        {"a\nb\n", 5, "a\nb\n", "a\nb\n"},
        {"a\n\n", 1, "a\n", "\n"},
        {"\n", 1, "\n", "\n"},
        {"\n\nb\nbaa", 3, "\n\nb\n", "\nb\nbaa"},
        {"x", 1, "x", "x"},
        {"", 3, "", ""},
    };
    size_t i;

    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        const char *text = cases[i].text;
        size_t len = strlen(text);
        size_t hlen = strlen(cases[i].head);
        size_t tlen = strlen(cases[i].tail);
        mem_file_t hf = {text, len, 0, 0};
        mem_file_t tf = {text, len, 0, 0};
        ssize_t g;

        /* Both tools print the bytes of their lines, so -n and -c agree on them. */
        g = sh_head_lines(mem_read_at, &hf, cases[i].want, case_got, SCRATCH, NULL, NULL, NULL, NULL);
        CHECK(g == (ssize_t)hlen);
        if (g > 0) {
            CHECK(memcmp(case_got, cases[i].head, hlen) == 0);
        }
        g = sh_tail_lines(mem_read_at, &tf, len, cases[i].want, case_got, SCRATCH, NULL, NULL, NULL);
        CHECK(g == (ssize_t)tlen);
        if (g > 0) {
            CHECK(memcmp(case_got, cases[i].tail, tlen) == 0);
        }
    }
    /* want 0 prints nothing for either tool, whatever the file holds. */
    {
        mem_file_t f = {"a\nb\n", 4, 0, 0};

        CHECK(sh_head_lines(mem_read_at, &f, 0, case_got, SCRATCH, NULL, NULL, NULL, NULL) == 0);
        CHECK(sh_tail_lines(mem_read_at, &f, f.len, 0, case_got, SCRATCH, NULL, NULL, NULL) == 0);
        CHECK(sh_head_bytes(mem_read_at, &f, 0, case_got, SCRATCH) == 0);
        CHECK(sh_tail_bytes(mem_read_at, &f, f.len, 0, case_got, SCRATCH) == 0);
    }
}

/* Builds a chunk-boundary file: kind 0 ends in a newline, 1 is unterminated, 2 is all newlines. */
static void fill_chunk_case(size_t len, int kind, size_t edge)
{
    size_t k;

    /* Plain bytes make one line; the stride keeps every run under SH_HT_LINE_MAX. */
    for (k = 0; k < len; k++) {
        case_file[k] = ((k % 64) == 63) ? '\n' : 'a';
    }
    if (kind == 0) {
        case_file[len - 1] = '\n';
    } else if (kind == 1) {
        case_file[len - 1] = 'z';  /* the last line is unterminated */
        if (edge < len) {
            case_file[edge] = '\n';  /* a newline exactly at a chunk edge */
        }
    } else if (kind == 2) {
        memset(case_file, '\n', len);  /* every byte ends a line */
    }
}

static void check_chunk_case(size_t len)
{
    const char *b = case_file;
    static const size_t byts[] = {0, 1, SH_HT_CHUNK - 1, SH_HT_CHUNK, SH_HT_CHUNK + 1};
    size_t w;
    size_t k;

    for (w = 0; w <= 3; w++) {
        check_lines_case(b, len, w);
    }
    for (k = 0; k < sizeof(byts) / sizeof(byts[0]); k++) {
        check_bytes_case(b, len, byts[k]);
    }
    /* More lines than the file holds is the walk's other exit: whole file or refused. */
    {
        mem_file_t walk = {b, len, 0, 0};
        size_t st = (size_t)-1;
        ssize_t g = sh_tail_lines(mem_read_at, &walk, len, 1000000, case_got, SCRATCH,
                                  NULL, NULL, &st);

        CHECK(g == (ssize_t)len);
        CHECK(st == 0);
        /* The walk never asks for more than a chunk, so a huge file is never pulled into memory. */
        CHECK(walk.max_once <= SH_HT_CHUNK);
        if (g == (ssize_t)len) {
            CHECK(memcmp(case_got, b, len) == 0);
        }

        walk.reads = 0;
        walk.max_once = 0;
        st = (size_t)-1;
        g = sh_tail_lines(mem_read_at, &walk, len, 1000000, case_got, len - 1, NULL, NULL, &st);
        CHECK(g == -1);
        CHECK(errno == ENAMETOOLONG);
    }
}

static void test_chunk_boundaries(void)
{
    size_t delta;

    for (delta = 0; delta < 3; delta++) {
        size_t size = SH_HT_CHUNK + delta - 1;  /* CHUNK-1, CHUNK and CHUNK+1 */
        int kind;
        size_t edge;

        for (kind = 0; kind < 3; kind++) {
            for (edge = 0; edge < 4; edge++) {
                fill_chunk_case(size, kind, edge * SH_HT_CHUNK - 1);
                check_chunk_case(size);
            }
        }
    }
}

/* The sink path: the answer leaves a chunk at a time, so the caller's buffer stays one chunk. */
static char sink_got[SCRATCH];
static size_t sink_len;
static size_t sink_calls;
static size_t sink_max_call;
static int sink_stop_after;  /* < 0 to never stop */

static int sink_take(void *ctx, const char *data, size_t len)
{
    (void)ctx;
    sink_calls++;
    if (len > sink_max_call) {
        sink_max_call = len;
    }
    if (sink_stop_after >= 0 && (int)sink_calls > sink_stop_after) {
        return 1;  /* the sink is full: the call must fail rather than press on */
    }
    if (sink_len + len > sizeof(sink_got)) {
        return 1;
    }
    memcpy(sink_got + sink_len, data, len);
    sink_len += len;
    return 0;
}

static void check_sink_case(const char *b, size_t len, size_t want)
{
    char expected[SCRATCH];
    size_t el = ref_tail(b, len, want, expected);
    mem_file_t f = {b, len, 0, 0};
    char one[SH_HT_CHUNK];
    ssize_t g;

    sink_len = 0;
    sink_calls = 0;
    sink_max_call = 0;
    sink_stop_after = -1;
    g = sh_tail_lines(mem_read_at, &f, len, want, one, sizeof(one), sink_take, NULL,
                      NULL);
    CHECK(g == (ssize_t)el);
    CHECK(sink_len == el);
    if (sink_len == el) {
        CHECK(memcmp(sink_got, expected, el) == 0);
    }
    /* The whole point of the sink: one chunk of caller buffer, however big the answer. */
    CHECK(sink_max_call <= SH_HT_CHUNK);
    if (el > SH_HT_CHUNK) {
        CHECK(sink_calls >= 2);
    }

    sink_len = 0;
    sink_calls = 0;
    sink_max_call = 0;
    sink_stop_after = -1;
    f.reads = 0;
    g = sh_head_lines(mem_read_at, &f, want, one, sizeof(one), sink_take, NULL, NULL,
                      NULL);
    CHECK(g == (ssize_t)ref_head(b, len, want, expected));
    CHECK(sink_len == (size_t)(g > 0 ? g : 0));
    CHECK(sink_max_call <= SH_HT_CHUNK);
}

static void test_sink(void)
{
    size_t i;

    /* The same cases as elsewhere, plus multi-chunk files that only fit via the sink. */
    for (i = 0; i < 3; i++) {
        size_t len = (i == 0) ? 1 : (i == 1 ? SH_HT_CHUNK : 3 * SH_HT_CHUNK + 7);
        size_t w;

        fill_chunk_case(len, (int)i, 0);
        for (w = 0; w <= 3; w++) {
            check_sink_case(case_file, len, w);
        }
        check_sink_case(case_file, len, 1000000);
    }
    /* A sink that stops mid-answer fails the call rather than reporting success. */
    {
        size_t len = 3 * SH_HT_CHUNK + 7;
        mem_file_t f = {NULL, len, 0, 0};
        char one[SH_HT_CHUNK];
        ssize_t g;

        fill_chunk_case(len, 0, 0);
        f.p = case_file;
        sink_len = 0;
        sink_calls = 0;
        sink_stop_after = 1;
        g = sh_tail_lines(mem_read_at, &f, len, 1000000, one, sizeof(one), sink_take,
                          NULL, NULL);
        CHECK(g == -1);
        CHECK(errno == ENOMEM);
    }
    /* A line over the bound is refused before the chunk carrying it is handed over. */
    {
        static char wide[SH_HT_LINE_MAX + SH_HT_CHUNK + 8];
        mem_file_t f;
        char one[SH_HT_CHUNK];
        size_t i2;
        ssize_t g;

        for (i2 = 0; i2 < sizeof(wide); i2++) {
            wide[i2] = 'q';
        }
        wide[sizeof(wide) - 1] = '\n';
        f.p = wide;
        f.len = sizeof(wide);
        f.reads = 0;
        f.max_once = 0;
        sink_len = 0;
        sink_calls = 0;
        sink_stop_after = -1;
        g = sh_tail_lines(mem_read_at, &f, f.len, 1, one, sizeof(one), sink_take, NULL,
                          NULL);
        CHECK(g == -1);
        CHECK(errno == E2BIG);
    }
}

/* Bounds, errors, and how far the reads are allowed to go. */
static void test_bounds_and_errors(void)
{
    /* head -c 4 of a larger file is read once, for four bytes. */
    {
        mem_file_t f = {"abcdefghijklmnopqrstuvwxyz", 26, 0, 0};

        CHECK(sh_head_bytes(mem_read_at, &f, 4, case_got, SCRATCH) == 4);
        CHECK(f.max_once <= 4);
    }
    /* The output cap is honored rather than overrun. */
    {
        mem_file_t f = {"abcdef", 6, 0, 0};

        CHECK(sh_head_bytes(mem_read_at, &f, 6, case_got, 3) == 3);
        CHECK(memcmp(case_got, "abc", 3) == 0);
    }
    /* head -n names the window it printed, newlines included. */
    {
        mem_file_t f = {"a\nb\nc\n", 6, 0, 0};
        size_t st = 9;
        size_t en = 9;

        CHECK(sh_head_lines(mem_read_at, &f, 2, case_got, SCRATCH, NULL, NULL, &st, &en) == 4);
        CHECK(st == 0 && en == 4);
        CHECK(memcmp(case_got, "a\nb\n", 4) == 0);
    }
    /* A line wider than the bound is E2BIG, not the prefix that fits. */
    {
        char *bigline = malloc(SH_HT_LINE_MAX + 8);
        mem_file_t f;

        memset(bigline, 'x', SH_HT_LINE_MAX + 8);
        memcpy(bigline, "ok\n", 3);
        bigline[SH_HT_LINE_MAX + 7] = '\n';
        f.p = bigline;
        f.len = SH_HT_LINE_MAX + 8;
        f.reads = 0;
        f.max_once = 0;
        CHECK(sh_head_lines(mem_read_at, &f, 2, case_got, SCRATCH, NULL, NULL, NULL, NULL) == -1);
        CHECK(errno == E2BIG);
        free(bigline);
    }
    /* tail -c reads only its window of a large file. */
    {
        static char big[1024 * 1024];
        static char win[64];
        mem_file_t f = {big, sizeof(big), 0, 0};

        memset(big, 'y', sizeof(big));
        CHECK(sh_tail_bytes(mem_read_at, &f, sizeof(big), 4, win, sizeof(win)) == 4);
        CHECK(f.max_once <= 4);
    }
    /* A file past the size bound, or a buffer past it, is refused before any read. */
    {
        mem_file_t f = {"abc", 3, 0, 0};

        CHECK(sh_tail_bytes(mem_read_at, &f, SH_HT_SIZE_MAX + 1, 3, case_got, SCRATCH) == -1);
        CHECK(errno == ERANGE);
        CHECK(sh_tail_bytes(mem_read_at, &f, 16, SH_HT_SIZE_MAX + 1, case_got, SCRATCH) == -1);
        CHECK(errno == ERANGE);
        CHECK(sh_tail_lines(mem_read_at, &f, SH_HT_SIZE_MAX + 1, 2, case_got, SCRATCH,
                            NULL, NULL, NULL) == -1);
        CHECK(errno == ERANGE);
        CHECK(sh_tail_lines(mem_read_at, &f, 4, 2, case_got, SH_HT_SIZE_MAX + 1,
                            NULL, NULL, NULL) == -1);
        CHECK(errno == ERANGE);
        CHECK(f.reads == 0);
        /* A cap too small for the answer is ENAMETOOLONG, never a truncated answer. */
        CHECK(sh_tail_lines(mem_read_at, &f, 4, 2, case_got, 1, NULL, NULL, NULL) == -1);
        CHECK(errno == ENAMETOOLONG);
    }
    /* The answer keeps its terminating newline, so it is 11 bytes here, not 10. */
    {
        mem_file_t f = {"aaaaaaaaaa\nbbbbbbbbbb\n", 22, 0, 0};

        CHECK(sh_tail_lines(mem_read_at, &f, f.len, 1, case_got, 10, NULL, NULL, NULL) == -1);
        CHECK(errno == ENAMETOOLONG);
        CHECK(sh_tail_lines(mem_read_at, &f, f.len, 1, case_got, 11, NULL, NULL, NULL) == 11);
        CHECK(memcmp(case_got, "bbbbbbbbbb\n", 11) == 0);
    }
    /* The walk stops once the answer is done: a 300 KB file asked for 2 lines is not read whole. */
    {
        static char big[300 * 1024];
        static char win[8 * SH_HT_CHUNK];
        mem_file_t f = {big, sizeof(big), 0, 0};
        size_t i;
        ssize_t got;

        for (i = 0; i < sizeof(big); i++) {
            big[i] = (char)(((i % 97) == 0) ? '\n' : 'x');
        }
        big[sizeof(big) - 1] = '\n';
        got = sh_tail_lines(mem_read_at, &f, f.len, 2, win, sizeof(win), NULL, NULL, NULL);
        CHECK(got > 0 && got < 4 * SH_HT_CHUNK);
        CHECK(f.max_once <= SH_HT_CHUNK);
    }
    /* A tail line wider than the bound is E2BIG rather than the prefix that fits. */
    {
        char *longline = malloc(SH_HT_LINE_MAX + 4);
        mem_file_t f;
        size_t i;

        for (i = 0; i < SH_HT_LINE_MAX + 3; i++) {
            longline[i] = 'z';
        }
        longline[SH_HT_LINE_MAX + 3] = '\n';
        f.p = longline;
        f.len = SH_HT_LINE_MAX + 4;
        f.reads = 0;
        f.max_once = 0;
        CHECK(sh_tail_lines(mem_read_at, &f, f.len, 1, case_got, SCRATCH, NULL, NULL, NULL) == -1);
        CHECK(errno == E2BIG);
        free(longline);
    }
    /* A read that fails reports the read's errno. */
    {
        sick_file_t s = {{"abcdefghij", 10, 0, 0}, 4};

        CHECK(sh_tail_bytes(sick_read_at, &s, 10, 4, case_got, SCRATCH) == -1);
        CHECK(errno == EIO);
    }
    {
        sick_file_t s = {{"aaa\nbbb\nccc\n", 12, 0, 0}, 8};

        CHECK(sh_tail_lines(sick_read_at, &s, 12, 3, case_got, SCRATCH, NULL, NULL, NULL) == -1);
        CHECK(errno == EIO);
    }
}

int main(void)
{
    test_worked_examples();
    test_against_reference();
    test_chunk_boundaries();
    test_sink();
    test_bounds_and_errors();

    if (failures == 0) {
        printf("PASS sh_headtail\n");
    } else {
        printf("FAIL sh_headtail: %d failures\n", failures);
    }
    return failures != 0;
}
