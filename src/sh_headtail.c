/* The window logic behind head and tail: which part of a stream to print, and how to get it
 * without reading the whole file. No Vita APIs, no stdout and no paths, so the host check
 * in scripts/test_sh_headtail.c runs the same code the server links. Bounds come back as
 * errno and the builtin reports them in the command's own voice. */

/* A line ends at a newline byte, or at end of file when the last line has no newline. The
 * empty file has no lines and "\n" has one empty line. head prints the first want lines
 * and tail the last want, each keeping its own newline, and both print the bytes of those
 * lines exactly as the file holds them, so a blank line prints as a blank line. */
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

/* Fills p with as much of [off, off+len) as the source gives, never asking for more than
 * SH_HT_CHUNK bytes at a time. Stops early at end of source and returns what arrived,
 * which is what a file that shrank under the read should print. */
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

/* Moves an answer of len bytes starting at off, either into out (bounded by out_cap and
 * refused before the first read when it cannot fit) or to the sink a chunk at a time, so
 * out needs only SH_HT_CHUNK bytes and the caller never has to promise an answer's size
 * to the heap. The sink is there because a tail -n 1000000 answer can be megabytes and
 * the server has one thread with every other session waiting on it. check_line watches
 * the SH_HT_LINE_MAX run as the bytes arrive rather than afterwards: a chunk handed to
 * the sink cannot be withdrawn, so a line over the bound has to be caught before the
 * chunk carrying it goes out. Returns the bytes produced, or -1 with errno set. */
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
        /* Nothing has left the buffer yet, so the bound is checked on the answer as a
         * whole here; the sink path below has to check it a chunk earlier for exactly
         * that reason. */
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

/* The -c answer for either tool: want bytes taken from off, capped by the buffer rather
 * than refused, because a byte window is verbatim and a short one is an honest answer
 * rather than a wrong one. head asks from 0 and tail from the end of the file. */
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
    /* Walks forward a byte at a time and stops just past the want'th newline, because
     * head prints bytes [0, i] for the index i of that newline; with fewer newlines than
     * were asked for the whole stream is the answer. off only grows and the source ends,
     * so the walk finishes at the latest at end of source. */
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
                /* Drained to its newline first, so a caller that reads on starts on a
                 * clean line rather than mid-line of the one it was refused. */
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
    /* The answer is the bytes read: a blank line is a newline in the output, and an
     * unterminated last line prints without one. */
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

/* end is file_size, or file_size - 1 when the last byte is a newline, because that newline
 * ends the last line rather than opening a blank one. The walk goes from end - 1 down to 0
 * and the answer starts one byte after the want'th newline it meets, running to file_size so
 * the answer keeps its own trailing newline. With fewer than want newlines there, every line
 * the file has is the answer and the start stays 0. */

/* Bytes are fetched in chunks rather than one by one, which changes only where a byte comes
 * from. pos is the first byte not yet read and starts at end, so each chunk is [from, pos)
 * with from < pos, and pos becomes from at the end of every pass. pos therefore strictly
 * decreases and the walk cannot repeat a byte or spin: it ends at the want'th newline, or at
 * pos == 0 once the whole range has been counted. */
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
    /* Both bounds are argument checks, so an over-long file or an impossible buffer is
     * refused before the first read rather than after the whole walk. */
    if (file_size > SH_HT_SIZE_MAX || out_cap > SH_HT_SIZE_MAX) {
        errno = ERANGE;
        return -1;
    }
    /* tail -n 0 prints nothing: an empty window at the end of the file, not a window
     * over all of it, which is what a start of 0 would otherwise read as. */
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
