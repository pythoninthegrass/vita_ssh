#include "vpk_zip.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#define EOCD_SIG 0x06054b50u
#define CDIR_SIG 0x02014b50u
#define LOCAL_SIG 0x04034b50u
#define EOCD_MIN 22
#define CDIR_FIXED 46
#define LOCAL_FIXED 30
#define MAX_COMMENT 0xFFFF
#define CHUNK (32 * 1024)

#define FLAG_ENCRYPTED 0x0001

struct vpk_zip {
    FILE *fp;
    long file_size;
    size_t count;
    vpk_entry_t *entries;
};

static uint16_t rd16(const unsigned char *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t rd32(const unsigned char *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* A name becomes a path under the app directory, so anything that could leave it is refused
 * here: an absolute path, a ".." component, a backslash, and ':' (a Vita device prefix). */
static int name_is_safe(const char *name)
{
    const char *p = name;

    if (name[0] == '\0' || name[0] == '/' || strpbrk(name, "\\:") != NULL) {
        return 0;
    }
    while (*p != '\0') {
        const char *end = strchr(p, '/');
        size_t len = (end != NULL) ? (size_t)(end - p) : strlen(p);

        if (len == 2 && p[0] == '.' && p[1] == '.') {
            return 0;
        }
        if (end == NULL) {
            break;
        }
        p = end + 1;
    }
    return 1;
}

static int read_at(FILE *fp, long off, void *dst, size_t len)
{
    if (fseek(fp, off, SEEK_SET) != 0) {
        return -1;
    }
    return fread(dst, 1, len, fp) == len ? 0 : -1;
}

/* Fills eocd with the record's bytes and returns its file offset, or -1. */
static long find_eocd(FILE *fp, long size, unsigned char eocd[EOCD_MIN])
{
    long window = size < (long)(EOCD_MIN + MAX_COMMENT) ? size : (long)(EOCD_MIN + MAX_COMMENT);
    long start = size - window;
    unsigned char *buf = malloc((size_t)window);
    long i, found = -1;

    if (buf == NULL) {
        return -2;
    }
    if (read_at(fp, start, buf, (size_t)window) != 0) {
        free(buf);
        return -3;
    }
    for (i = window - EOCD_MIN; i >= 0; i--) {
        if (rd32(buf + i) == EOCD_SIG) {
            memcpy(eocd, buf + i, EOCD_MIN);
            found = start + i;
            break;
        }
    }
    free(buf);
    return found;
}

vpk_err_t vpk_zip_open(vpk_zip_t **out, const char *path)
{
    unsigned char eocd[EOCD_MIN];
    unsigned char *cd = NULL;
    vpk_zip_t *z;
    size_t cd_size, pos = 0, i;
    uint32_t cd_off;
    long eocd_off;
    vpk_err_t err = VPK_E_CORRUPT;

    *out = NULL;
    z = calloc(1, sizeof(*z));
    if (z == NULL) {
        return VPK_E_NOMEM;
    }
    z->fp = fopen(path, "rb");
    if (z->fp == NULL) {
        free(z);
        return VPK_E_IO;
    }
    if (fseek(z->fp, 0, SEEK_END) != 0 || (z->file_size = ftell(z->fp)) < 0) {
        err = VPK_E_IO;
        goto fail;
    }
    eocd_off = find_eocd(z->fp, z->file_size, eocd);
    if (eocd_off == -2) {
        err = VPK_E_NOMEM;
        goto fail;
    }
    if (eocd_off == -3) {
        err = VPK_E_IO;
        goto fail;
    }
    if (eocd_off < 0) {
        err = VPK_E_NOT_ZIP;
        goto fail;
    }
    if (rd16(eocd + 4) != 0 || rd16(eocd + 6) != 0 || rd16(eocd + 8) != rd16(eocd + 10) ||
        rd16(eocd + 10) == 0xFFFF || rd32(eocd + 12) == 0xFFFFFFFFu || rd32(eocd + 16) == 0xFFFFFFFFu) {
        err = VPK_E_UNSUPPORTED;
        goto fail;
    }
    z->count = rd16(eocd + 10);
    cd_size = rd32(eocd + 12);
    cd_off = rd32(eocd + 16);
    if ((long)cd_off < 0 || (long)cd_off + (long)cd_size > eocd_off) {
        goto fail;
    }
    cd = malloc(cd_size > 0 ? cd_size : 1);
    z->entries = calloc(z->count > 0 ? z->count : 1, sizeof(vpk_entry_t));
    if (cd == NULL || z->entries == NULL) {
        err = VPK_E_NOMEM;
        goto fail;
    }
    if (read_at(z->fp, (long)cd_off, cd, cd_size) != 0) {
        err = VPK_E_IO;
        goto fail;
    }

    for (i = 0; i < z->count; i++) {
        vpk_entry_t *e = &z->entries[i];
        uint16_t flags, method, nlen, xlen, clen;

        if (pos + CDIR_FIXED > cd_size || rd32(cd + pos) != CDIR_SIG) {
            goto fail;
        }
        flags = rd16(cd + pos + 8);
        method = rd16(cd + pos + 10);
        nlen = rd16(cd + pos + 28);
        xlen = rd16(cd + pos + 30);
        clen = rd16(cd + pos + 32);
        if (pos + CDIR_FIXED + nlen + xlen + clen > cd_size) {
            goto fail;
        }
        if (nlen > VPK_NAME_MAX) {
            err = VPK_E_BAD_NAME;
            goto fail;
        }
        memcpy(e->name, cd + pos + CDIR_FIXED, nlen);
        e->name[nlen] = '\0';
        if (strlen(e->name) != nlen || !name_is_safe(e->name)) {
            err = VPK_E_BAD_NAME;
            goto fail;
        }
        if ((flags & FLAG_ENCRYPTED) != 0 || (method != 0 && method != 8) ||
            rd32(cd + pos + 20) == 0xFFFFFFFFu || rd32(cd + pos + 24) == 0xFFFFFFFFu ||
            rd32(cd + pos + 42) == 0xFFFFFFFFu) {
            err = VPK_E_UNSUPPORTED;
            goto fail;
        }
        e->method = method;
        e->crc = rd32(cd + pos + 16);
        e->csize = rd32(cd + pos + 20);
        e->usize = rd32(cd + pos + 24);
        e->local_offset = rd32(cd + pos + 42);
        e->is_dir = nlen > 0 && e->name[nlen - 1] == '/';
        if ((long)e->local_offset + LOCAL_FIXED + (long)e->csize > z->file_size) {
            goto fail;
        }
        pos += CDIR_FIXED + nlen + xlen + clen;
    }
    free(cd);
    *out = z;
    return VPK_OK;

fail:
    free(cd);
    vpk_zip_close(z);
    return err;
}

void vpk_zip_close(vpk_zip_t *z)
{
    if (z == NULL) {
        return;
    }
    if (z->fp != NULL) {
        fclose(z->fp);
    }
    free(z->entries);
    free(z);
}

size_t vpk_zip_count(const vpk_zip_t *z)
{
    return z->count;
}

const vpk_entry_t *vpk_zip_entry(const vpk_zip_t *z, size_t index)
{
    return index < z->count ? &z->entries[index] : NULL;
}

const vpk_entry_t *vpk_zip_find(const vpk_zip_t *z, const char *name)
{
    size_t i;

    for (i = 0; i < z->count; i++) {
        if (strcmp(z->entries[i].name, name) == 0) {
            return &z->entries[i];
        }
    }
    return NULL;
}

/* The local header's own name/extra lengths locate the data; they may differ from the
 * central directory's. */
static vpk_err_t locate_data(vpk_zip_t *z, const vpk_entry_t *e, long *data_off)
{
    unsigned char lh[LOCAL_FIXED];

    if (read_at(z->fp, (long)e->local_offset, lh, sizeof(lh)) != 0) {
        return VPK_E_IO;
    }
    if (rd32(lh) != LOCAL_SIG) {
        return VPK_E_CORRUPT;
    }
    *data_off = (long)e->local_offset + LOCAL_FIXED + rd16(lh + 26) + rd16(lh + 28);
    if (*data_off + (long)e->csize > z->file_size) {
        return VPK_E_CORRUPT;
    }
    return VPK_OK;
}

static vpk_err_t extract_stored(vpk_zip_t *z, const vpk_entry_t *e, vpk_sink_fn sink, void *ctx,
                                unsigned char *in, uLong *crc)
{
    uint32_t left = e->csize;

    if (e->csize != e->usize) {
        return VPK_E_CORRUPT;
    }
    while (left > 0) {
        size_t n = left < CHUNK ? left : CHUNK;

        if (fread(in, 1, n, z->fp) != n) {
            return VPK_E_IO;
        }
        *crc = crc32(*crc, in, (uInt)n);
        if (sink(ctx, in, n) != 0) {
            return VPK_E_SINK;
        }
        left -= (uint32_t)n;
    }
    return VPK_OK;
}

static vpk_err_t extract_deflated(vpk_zip_t *z, const vpk_entry_t *e, vpk_sink_fn sink, void *ctx,
                                  unsigned char *in, unsigned char *outbuf, uLong *crc)
{
    z_stream zs;
    uint32_t left = e->csize;
    uint32_t produced = 0;
    int rc = Z_OK;
    vpk_err_t err = VPK_OK;

    memset(&zs, 0, sizeof(zs));
    if (inflateInit2(&zs, -15) != Z_OK) {
        return VPK_E_NOMEM;
    }
    while (rc != Z_STREAM_END) {
        if (zs.avail_in == 0) {
            size_t n = left < CHUNK ? left : CHUNK;

            if (n == 0) {
                err = VPK_E_CORRUPT;
                break;
            }
            if (fread(in, 1, n, z->fp) != n) {
                err = VPK_E_IO;
                break;
            }
            left -= (uint32_t)n;
            zs.next_in = in;
            zs.avail_in = (uInt)n;
        }
        zs.next_out = outbuf;
        zs.avail_out = CHUNK;
        rc = inflate(&zs, Z_NO_FLUSH);
        if (rc != Z_OK && rc != Z_STREAM_END) {
            err = VPK_E_CORRUPT;
            break;
        }
        if (zs.avail_out < CHUNK) {
            size_t n = CHUNK - zs.avail_out;

            produced += (uint32_t)n;
            if (produced > e->usize) {
                err = VPK_E_CORRUPT;
                break;
            }
            *crc = crc32(*crc, outbuf, (uInt)n);
            if (sink(ctx, outbuf, n) != 0) {
                err = VPK_E_SINK;
                break;
            }
        }
    }
    inflateEnd(&zs);
    if (err == VPK_OK && produced != e->usize) {
        err = VPK_E_CORRUPT;
    }
    return err;
}

vpk_err_t vpk_zip_extract(vpk_zip_t *z, const vpk_entry_t *e, vpk_sink_fn sink, void *ctx)
{
    unsigned char *in, *outbuf;
    uLong crc = crc32(0L, Z_NULL, 0);
    long data_off;
    vpk_err_t err;

    err = locate_data(z, e, &data_off);
    if (err != VPK_OK) {
        return err;
    }
    if (fseek(z->fp, data_off, SEEK_SET) != 0) {
        return VPK_E_IO;
    }
    in = malloc(CHUNK);
    outbuf = malloc(CHUNK);
    if (in == NULL || outbuf == NULL) {
        free(in);
        free(outbuf);
        return VPK_E_NOMEM;
    }
    err = (e->method == 0) ? extract_stored(z, e, sink, ctx, in, &crc)
                           : extract_deflated(z, e, sink, ctx, in, outbuf, &crc);
    free(in);
    free(outbuf);
    if (err == VPK_OK && (uint32_t)crc != e->crc) {
        err = VPK_E_CRC;
    }
    return err;
}

const char *vpk_zip_strerror(vpk_err_t err)
{
    switch (err) {
    case VPK_OK:
        return "ok";
    case VPK_E_IO:
        return "cannot read file";
    case VPK_E_NOMEM:
        return "out of memory";
    case VPK_E_NOT_ZIP:
        return "not a zip file";
    case VPK_E_CORRUPT:
        return "corrupt package";
    case VPK_E_UNSUPPORTED:
        return "unsupported zip feature";
    case VPK_E_BAD_NAME:
        return "unsafe file name in package";
    case VPK_E_CRC:
        return "checksum mismatch";
    case VPK_E_SINK:
        return "write failed";
    }
    return "unknown error";
}
