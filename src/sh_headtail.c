/* Window logic for head and tail, no Vita APIs; notes in docs/shell-text-tools.md. */

/* A line ends at a newline or at EOF; head and tail print whole lines verbatim. */
#include "sh_headtail.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* Reads through the source at an absolute offset; 0 from read_at means end of source. */
static ssize_t pull(sh_ht_seek_read_fn read_at, void *ctx, char *p, size_t n,
                    size_t off)
{
    if (n == 0) {
        return 0;
    }
    return read_at(ctx, p, n, off);
}

/* Fills p from [off, off+len) in reads of at most SH_HT_CHUNK bytes; stops early at EOF. */
static ssize_t pull_chunked(sh_ht_seek_read_fn read_at, void *ctx, char *p, size_t off,
                            size_t len)
{
    size_t got = 0;

    while (got < len) {
        size_t take = (len - got > SH_HT_CHUNK) ? SH_HT_CHUNK : (len - got);
        ssize_t n = pull(read_at, ctx, p + got, take, off + got);

        if (n < 0) {
            return -1;
        }
        if (n == 0) {
            break;
        }
        got += (size_t)n;
    }
    return (ssize_t)got;
}

/* Moves len bytes from off into out or the sink in chunks, checking the line bound en route. */
static ssize_t deliver(sh_ht_seek_read_fn read_at, void *ctx, size_t off, size_t len,
                       char *out, size_t out_cap, sh_ht_sink_fn sink, void *sink_ctx,
                       int check_line)
{
    size_t done = 0;
    size_t run = 0;
    size_t j;
    ssize_t got;

    if (!sink) {
        if (len > out_cap) {
            errno = ENAMETOOLONG;
            return -1;
        }
        got = pull_chunked(read_at, ctx, out, off, len);
        if (got < 0) {
            return -1;
        }
        /* Nothing has left the buffer yet, so the bound is checked on the whole answer here. */
        if (check_line) {
            for (j = 0; j < (size_t)got; j++) {
                if (out[j] == '\n') {
                    run = 0;
                    continue;
                }
                if (++run > SH_HT_LINE_MAX) {
                    errno = E2BIG;
                    return -1;
                }
            }
        }
        return got;
    }
    while (done < len) {
        size_t take = (len - done > SH_HT_CHUNK) ? SH_HT_CHUNK : (len - done);

        got = 0;
        while ((size_t)got < take) {
            ssize_t n = pull(read_at, ctx, out + got, take - got, off + done + got);

            if (n < 0) {
                return -1;
            }
            if (n == 0) {
                break;  /* the source ended: what has gone out is the whole answer */
            }
            got += n;
        }
        for (j = 0; j < (size_t)got; j++) {
            if (out[j] == '\n') {
                run = 0;
                continue;
            }
            if (check_line && ++run > SH_HT_LINE_MAX) {
                errno = E2BIG;
                return -1;
            }
        }
        if (got != 0 && sink(sink_ctx, out, got) != 0) {
            errno = ENOMEM;
            return -1;
        }
        done += (size_t)got;
        if ((size_t)got != take) {
            break;
        }
    }
    return (ssize_t)done;
}

/* The -c answer for head and tail: want bytes from off, capped by the buffer, not refused. */
static ssize_t byte_window(sh_ht_seek_read_fn read_at, void *ctx, size_t off,
                           size_t want, char *out, size_t out_cap)
{
    if (want > out_cap) {
        want = out_cap;
    }
    return deliver(read_at, ctx, off, want, out, out_cap, NULL, NULL, 0);
}

ssize_t sh_head_bytes(sh_ht_seek_read_fn read_at, void *ctx, size_t want,
                      char *out, size_t out_cap)
{
    return byte_window(read_at, ctx, 0, want, out, out_cap);
}

ssize_t sh_head_lines(sh_ht_seek_read_fn read_at, void *ctx, size_t want,
                      char *out, size_t out_cap, sh_ht_sink_fn sink, void *sink_ctx,
                      size_t *start, size_t *end)
{
    size_t off = 0;
    size_t lines_seen = 0;
    size_t run = 0;  /* bytes in the line being read, against SH_HT_LINE_MAX */
    ssize_t got;

    if (start != NULL) {
        *start = 0;
    }
    if (end != NULL) {
        *end = 0;
    }
    /* head -n 0 prints nothing: an empty window at the start of the stream. */
    if (want == 0) {
        return 0;
    }
    /* Walks forward to just past the want'th newline; with fewer newlines, all of it. */
    for (;;) {
        char c;
        ssize_t n = pull(read_at, ctx, &c, 1, off);

        if (n < 0) {
            return -1;
        }
        if (n == 0) {
            break;  /* end of source: a last line with no newline is still a line */
        }
        off++;
        if (c != '\n') {
            if (++run > SH_HT_LINE_MAX) {
                /* Drain to the newline first so a caller that reads on starts on a clean line. */
                do {
                    n = pull(read_at, ctx, &c, 1, off);
                    if (n < 0) {
                        return -1;
                    }
                    off += (size_t)n;
                } while (n == 1 && c != '\n');
                errno = E2BIG;
                return -1;
            }
            continue;
        }
        run = 0;
        lines_seen++;
        if (lines_seen == want) {
            break;
        }
    }
    /* The answer is the bytes read: a blank line is a newline, a last line may lack one. */
    got = deliver(read_at, ctx, 0, off, out, out_cap, sink, sink_ctx, 1);
    if (got < 0) {
        return -1;
    }
    if (end != NULL) {
        *end = sink ? off : (size_t)got;
    }
    return got;
}

ssize_t sh_tail_bytes(sh_ht_seek_read_fn read_at, void *ctx, size_t file_size,
                      size_t want, char *out, size_t out_cap)
{
    size_t start;

    if (file_size > SH_HT_SIZE_MAX || want > SH_HT_SIZE_MAX) {
        errno = ERANGE;
        return -1;
    }
    if (want > file_size) {
        want = file_size;
    }
    start = file_size - want;
    return byte_window(read_at, ctx, start, want, out, out_cap);
}

/* Finds where tail's answer begins, by counting newlines backwards from end. */

/* end skips a final newline, which ends the last line; answer starts after want'th newline. */

/* Reads backwards in chunks; pos strictly decreases, so the walk ends at want newlines or 0. */
static int tail_scan(sh_ht_seek_read_fn read_at, void *ctx, size_t file_size,
                     size_t want, char *chunk, size_t *start)
{
    size_t end;
    size_t pos;
    size_t count = 0;
    char last;
    ssize_t n;

    *start = 0;
    n = pull(read_at, ctx, &last, 1, file_size - 1);
    if (n < 0) {
        return -1;
    }
    end = (n == 1 && last == '\n') ? file_size - 1 : file_size;
    pos = end;

    while (pos > 0) {
        size_t from = (pos > SH_HT_CHUNK) ? pos - SH_HT_CHUNK : 0;
        size_t take = pos - from;
        size_t i;

        n = pull(read_at, ctx, chunk, take, from);
        if (n < 0) {
            return -1;
        }
        if ((size_t)n < take) {
            /* The file is smaller than the size handed in, so count only what exists. */
            take = (size_t)n;
            if (take == 0) {
                break;
            }
        }
        for (i = take; i > 0; i--) {
            if (chunk[i - 1] != '\n') {
                continue;
            }
            count++;
            if (count == want) {
                *start = from + i;  /* one past the newline that opens the answer */
                return 0;
            }
        }
        pos = from;  /* every byte of the chunk is counted, so the walk moves down to it */
    }
    return 0;  /* fewer lines in the file than were asked: all of it is the answer */
}

ssize_t sh_tail_lines(sh_ht_seek_read_fn read_at, void *ctx, size_t file_size,
                      size_t want, char *out, size_t out_cap, sh_ht_sink_fn sink,
                      void *sink_ctx, size_t *start_out)
{
    char chunk[SH_HT_CHUNK];
    size_t start = 0;

    if (start_out != NULL) {
        *start_out = 0;
    }
    /* Both bounds are argument checks, refused before the first read rather than after the walk. */
    if (file_size > SH_HT_SIZE_MAX || out_cap > SH_HT_SIZE_MAX) {
        errno = ERANGE;
        return -1;
    }
    /* tail -n 0 prints nothing: an empty window at the end of the file, not one over all of it. */
    if (want == 0) {
        if (start_out != NULL) {
            *start_out = file_size;
        }
        return 0;
    }
    if (file_size == 0) {
        return 0;
    }
    if (tail_scan(read_at, ctx, file_size, want, chunk, &start) < 0) {
        return -1;
    }
    if (start_out != NULL) {
        *start_out = start;  /* an empty answer begins where it would have begun */
    }
    return deliver(read_at, ctx, start, file_size - start, out, out_cap, sink, sink_ctx,
                   1);
}
