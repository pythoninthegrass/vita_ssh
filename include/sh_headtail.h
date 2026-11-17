/* Line and byte windows for head and tail: no I/O and no Vita APIs, so host tests run this code. */
#ifndef SH_HEADTAIL_H
#define SH_HEADTAIL_H

#include <stddef.h>
#include <sys/types.h>

/* Longest line either tool accepts, shared with grep; a longer line is reported, not truncated. */
#define SH_HT_LINE_MAX 4096

/* Longest file tail reads backwards, and the most bytes tail -c can be asked for. */
#define SH_HT_SIZE_MAX (16 * 1024 * 1024)

/* Bytes either tool moves per read, and the buffer size needed when a sink takes the answer. */
#define SH_HT_CHUNK 4096

/* A seekable source: reads n bytes at offset off into p; returns the count (0 at EOF) or -1. */
typedef ssize_t (*sh_ht_seek_read_fn)(void *ctx, void *p, size_t n, size_t off);

/* Receives each chunk of an answer too big to hold: 0 carries on, nonzero fails with ENOMEM. */
typedef int (*sh_ht_sink_fn)(void *ctx, const char *data, size_t len);

/* Puts the first want bytes in out; returns bytes written (at most out_cap) or -1 with errno. */
ssize_t sh_head_bytes(sh_ht_seek_read_fn read_at, void *ctx, size_t want,
                      char *out, size_t out_cap);

/* Puts the first want whole lines in out; returns bytes written or -1 (E2BIG, ENAMETOOLONG). */
ssize_t sh_head_lines(sh_ht_seek_read_fn read_at, void *ctx, size_t want,
                      char *out, size_t out_cap, sh_ht_sink_fn sink, void *sink_ctx,
                      size_t *start, size_t *end);

/* Puts the last want bytes of a file_size-byte file in out; returns bytes or -1. */
ssize_t sh_tail_bytes(sh_ht_seek_read_fn read_at, void *ctx, size_t file_size,
                      size_t want, char *out, size_t out_cap);

/* Puts the last want whole lines in out; returns bytes written or -1 (ERANGE, E2BIG, ...). */
ssize_t sh_tail_lines(sh_ht_seek_read_fn read_at, void *ctx, size_t file_size,
                      size_t want, char *out, size_t out_cap, sh_ht_sink_fn sink,
                      void *sink_ctx, size_t *start);

#endif
