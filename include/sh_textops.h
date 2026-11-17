/* Counting, ordering and collapsing for wc, sort and uniq: no I/O and no Vita APIs. */
#ifndef SH_TEXTOPS_H
#define SH_TEXTOPS_H

#include <stddef.h>
#include <sys/types.h>

/* Bounds on total input bytes, lines held and longest line; each is named in the tool's error. */
#define SH_TEXTOPS_BYTES_MAX (4 * 1024 * 1024)
#define SH_TEXTOPS_LINES_MAX 200000
#define SH_TEXTOPS_LINE_MAX 65536

/* The most digits a -n key may carry, integer and fraction together. */
#define SH_TEXTOPS_DIGITS_MAX 32

/* A byte source: copies up to want bytes into p, returns the count (0 at end), or -1 with errno. */
typedef ssize_t (*sh_textops_fill_fn)(void *ctx, char *p, size_t want);

/* A line sink: gets the line without newline and uniq's run length; nonzero stops the call. */
typedef int (*sh_textops_line_fn)(void *ctx, const char *line, size_t len,
                                  unsigned long count);

/* One input through wc: all three counts are kept; 0, or -1 with errno EFBIG or the fill's. */
typedef struct {
    int want_lines;
    int want_words;
    int want_bytes;
    unsigned long lines;
    unsigned long words;
    unsigned long bytes;
} sh_wc_t;

int sh_wc(sh_textops_fill_fn fill, void *ctx, sh_wc_t *wc);

/* sort's verdict: negative, zero or positive, or SH_TEXTOPS_BAD_KEY for an over-long -n key. */
#define SH_TEXTOPS_BAD_KEY 4099

/* The sort comparison: unsigned byte order, or with numeric the leading number then byte order. */
int sh_cmp(const char *a, size_t al, const char *b, size_t bl, int numeric);

/* Orders the whole input and emits lines in order; 0, or -1 with errno; -u drops equal keys. */
int sh_sort(sh_textops_fill_fn fill, void *ctx, size_t size, int numeric, int reverse,
            int uniq, sh_textops_line_fn emit, void *emit_ctx);

/* Collapses runs of identical adjacent lines and emits one per run with its length; 0 or -1. */
int sh_uniq(sh_textops_fill_fn fill, void *ctx, size_t size, int show_count,
            int dups_only, int singles_only, sh_textops_line_fn emit, void *emit_ctx);

#endif
