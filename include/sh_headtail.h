/* Line and byte windows for head and tail: no I/O and no Vita APIs, so the same
 * code the server links is what the host check exercises. */
#ifndef SH_HEADTAIL_H
#define SH_HEADTAIL_H

#include <stddef.h>
#include <sys/types.h>

/* Longest line either tool accepts, shared with grep so one bound is stated once.
 * A longer line is reported, never truncated into a partial answer. */
#define SH_HT_LINE_MAX 4096

/* Longest file tail reads backwards, and the most bytes tail -c can be asked for. */
#define SH_HT_SIZE_MAX (16 * 1024 * 1024)

/* Bytes either tool moves per read, and the size of the buffer they need when a sink
 * carries the answer away. */
#define SH_HT_CHUNK 4096

/* A seekable source: reads n bytes at absolute offset off into p, returns how many
 * it took (0 at end of file), or -1 with errno set. */
typedef ssize_t (*sh_ht_seek_read_fn)(void *ctx, void *p, size_t n, size_t off);

/* Where an answer goes when the caller cannot hold it: called with each chunk, and
 * returns 0 to carry on or nonzero to stop, which fails the call with ENOMEM. Pass
 * NULL where the answer should accumulate in out instead. */
typedef int (*sh_ht_sink_fn)(void *ctx, const char *data, size_t len);

/* Puts the first want bytes of the source in out. Returns the bytes written -- fewer
 * than want only at end of file, and never more than out_cap -- or -1 with errno set. */
ssize_t sh_head_bytes(sh_ht_seek_read_fn read_at, void *ctx, size_t want,
                      char *out, size_t out_cap);

/* Puts the first want whole lines in out, each exactly as the file has it and each
 * keeping its own newline, so what the source holds between the two offsets it returns
 * is what head prints. Returns the bytes written, and stores where the window begins and
 * ends in *start and *end when they are not NULL, or -1 with errno E2BIG when a line is
 * longer than SH_HT_LINE_MAX (the stream is drained to the next newline first, so a
 * caller reading on gets a clean line), ENAMETOOLONG when the window needs more than
 * out_cap and no sink was given, or the read's errno. With a sink the answer leaves a
 * chunk at a time and out needs only SH_HT_CHUNK bytes. */
ssize_t sh_head_lines(sh_ht_seek_read_fn read_at, void *ctx, size_t want,
                      char *out, size_t out_cap, sh_ht_sink_fn sink, void *sink_ctx,
                      size_t *start, size_t *end);

/* Puts the last want bytes of a file of file_size bytes in out. Returns the bytes
 * written, or -1 with errno set: ERANGE when file_size or want is beyond
 * SH_HT_SIZE_MAX, or the read's errno. */
ssize_t sh_tail_bytes(sh_ht_seek_read_fn read_at, void *ctx, size_t file_size,
                      size_t want, char *out, size_t out_cap);

/* Puts the last want whole lines of a file of file_size bytes in out, each exactly as
 * the file has it, so the bytes are what tail prints: a file that ends in a newline ends
 * its last line with it and does not gain a blank line, and one that does not end in a
 * newline prints that last line unterminated. The file is read backwards in SH_HT_CHUNK
 * steps and stopped as soon as want lines are held, so the bytes read never exceed the
 * answer plus one chunk. Returns the bytes written, and stores where the window begins in
 * *start when it is not NULL, or -1 with errno set: ERANGE when file_size is beyond
 * SH_HT_SIZE_MAX, ENAMETOOLONG when the lines kept need more than out_cap and no sink was
 * given, E2BIG when a line is longer than SH_HT_LINE_MAX, or the read's errno. With a
 * sink the answer leaves a chunk at a time, out needs only SH_HT_CHUNK bytes, and every
 * line is checked before its chunk goes out -- a wide line is refused rather than
 * printed in part. */
ssize_t sh_tail_lines(sh_ht_seek_read_fn read_at, void *ctx, size_t file_size,
                      size_t want, char *out, size_t out_cap, sh_ht_sink_fn sink,
                      void *sink_ctx, size_t *start);

#endif
