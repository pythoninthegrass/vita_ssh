/* Counting, ordering and collapsing for wc, sort and uniq: no I/O and no Vita APIs, so
 * the same code the server links is what the host check exercises. */
#ifndef SH_TEXTOPS_H
#define SH_TEXTOPS_H

#include <stddef.h>
#include <sys/types.h>

/* Total input bytes one input may hold, the most lines it may hold, and the longest line
 * the two tools that keep lines accept. Each is named in the error the tool prints when
 * the input passes it. */
#define SH_TEXTOPS_BYTES_MAX (4 * 1024 * 1024)
#define SH_TEXTOPS_LINES_MAX 200000
#define SH_TEXTOPS_LINE_MAX 65536

/* The most digits a -n key may carry, integer and fraction together. */
#define SH_TEXTOPS_DIGITS_MAX 32

/* A byte source: copies up to want bytes into p, returns how many it took (0 at end of
 * input), or -1 with errno set. */
typedef ssize_t (*sh_textops_fill_fn)(void *ctx, char *p, size_t want);

/* A line sink: called with the line body without its newline and with the run length for
 * uniq, and returns 0 to carry on. Nonzero stops the call, which fails with that code. */
typedef int (*sh_textops_line_fn)(void *ctx, const char *line, size_t len,
                                  unsigned long count);

/* One input through wc. All three counts are kept whether or not they were asked for,
 * because the field width comes from the largest byte count across every input and the
 * caller prints the rows. Returns 0, or -1 with errno EFBIG past SH_TEXTOPS_BYTES_MAX or
 * the fill's errno. */
typedef struct {
    int want_lines;
    int want_words;
    int want_bytes;
    unsigned long lines;
    unsigned long words;
    unsigned long bytes;
} sh_wc_t;

int sh_wc(sh_textops_fill_fn fill, void *ctx, sh_wc_t *wc);

/* sort's verdict from sh_cmp: negative, zero or positive, or SH_TEXTOPS_BAD_KEY when a -n
 * key carries more digits than SH_TEXTOPS_DIGITS_MAX and no comparison is honest. */
#define SH_TEXTOPS_BAD_KEY 4099

/* The comparison sort uses: byte order (unsigned memcmp, a proper prefix first), or with
 * numeric the leading numeric value first and byte order as the tie-break. */
int sh_cmp(const char *a, size_t al, const char *b, size_t bl, int numeric);

/* Orders the whole input and hands each line to emit in order, with count 1. Returns 0,
 * or -1 with errno set: EFBIG when the input passes one of the bounds (the caller names
 * which from the size passed in), ENOMEM, EILSEQ for an unrepresentable -n key, or the
 * fill's errno. Equal lines keep their input order, and with uniq one of two neighbouring
 * lines whose key compares equal is dropped -- the later one in input order. */
int sh_sort(sh_textops_fill_fn fill, void *ctx, size_t size, int numeric, int reverse,
            int uniq, sh_textops_line_fn emit, void *emit_ctx);

/* Collapses runs of adjacent byte-identical lines and hands one line per run to emit, in
 * input order, with the run's length. Returns 0, or -1 with errno EFBIG past a bound,
 * ENOMEM, or the fill's errno. */
int sh_uniq(sh_textops_fill_fn fill, void *ctx, size_t size, int show_count,
            int dups_only, int singles_only, sh_textops_line_fn emit, void *emit_ctx);

#endif
