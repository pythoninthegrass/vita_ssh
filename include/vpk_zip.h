/*
 * Minimal zip reader for the `vpk` builtin. Pure C plus zlib, no sce* calls, so it also
 * builds on the host for scripts/test_vpk_zip.c.
 *
 * Reads the central directory, then streams one entry at a time through a sink. Stored and
 * deflated entries only; zip64, encryption and data descriptors that disagree with the
 * central directory are refused rather than guessed at.
 */
#ifndef VPK_ZIP_H
#define VPK_ZIP_H

#include <stddef.h>
#include <stdint.h>

#define VPK_NAME_MAX 255

typedef enum {
    VPK_OK = 0,
    VPK_E_IO,          /* open/read/seek failed */
    VPK_E_NOMEM,
    VPK_E_NOT_ZIP,     /* no end-of-central-directory record */
    VPK_E_CORRUPT,     /* offsets or sizes outside the file, bad local header */
    VPK_E_UNSUPPORTED, /* zip64, encryption, multi-disk, or a method other than store/deflate */
    VPK_E_BAD_NAME,    /* absolute, ".." component, backslash, device prefix, too long */
    VPK_E_CRC,         /* extracted data does not match the recorded CRC-32 */
    VPK_E_SINK         /* the sink callback refused the data */
} vpk_err_t;

typedef struct {
    char name[VPK_NAME_MAX + 1];
    uint32_t crc;
    uint32_t csize;
    uint32_t usize;
    uint32_t local_offset;
    uint16_t method;
    int is_dir;
} vpk_entry_t;

typedef struct vpk_zip vpk_zip_t;

/* Return 0 to continue, nonzero to abort the extraction (reported as VPK_E_SINK). */
typedef int (*vpk_sink_fn)(void *ctx, const unsigned char *data, size_t len);

vpk_err_t vpk_zip_open(vpk_zip_t **out, const char *path);
void vpk_zip_close(vpk_zip_t *z);

size_t vpk_zip_count(const vpk_zip_t *z);
const vpk_entry_t *vpk_zip_entry(const vpk_zip_t *z, size_t index);
/* NULL when no entry has exactly this name. */
const vpk_entry_t *vpk_zip_find(const vpk_zip_t *z, const char *name);

/* Streams the entry's uncompressed bytes to sink in chunks and verifies the CRC-32 once
 * the last chunk is out; a CRC failure is reported after the sink has seen the data, so
 * a caller writing a file must discard it on any nonzero return. */
vpk_err_t vpk_zip_extract(vpk_zip_t *z, const vpk_entry_t *e, vpk_sink_fn sink, void *ctx);

const char *vpk_zip_strerror(vpk_err_t err);

#endif
