#include "vpk_head.h"

#include <stdint.h>
#include <string.h>

#define HDR_CONTENT_ID 0x30
#define HDR_INFO_OFF 0x08
#define HDR_INFO_LEN 0x10
#define HDR_MAC1_LEN 0xD0
#define HDR_MAC2_OUT 0xD4
#define HDR_MAC3_LEN 0xE8
#define MAC_SIZE 16

typedef struct {
    uint32_t h[5];
    uint64_t bits;
    unsigned char block[64];
    size_t used;
} sha1_t;

static uint32_t rol(uint32_t v, unsigned n)
{
    return (v << n) | (v >> (32 - n));
}

static void sha1_block(sha1_t *c, const unsigned char *p)
{
    uint32_t w[80], a = c->h[0], b = c->h[1], cc = c->h[2], d = c->h[3], e = c->h[4];
    int i;

    for (i = 0; i < 16; i++) {
        w[i] = ((uint32_t)p[i * 4] << 24) | ((uint32_t)p[i * 4 + 1] << 16) |
               ((uint32_t)p[i * 4 + 2] << 8) | p[i * 4 + 3];
    }
    for (i = 16; i < 80; i++) {
        w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    }
    for (i = 0; i < 80; i++) {
        uint32_t f, k, t;

        if (i < 20) {
            f = (b & cc) | (~b & d);
            k = 0x5A827999u;
        } else if (i < 40) {
            f = b ^ cc ^ d;
            k = 0x6ED9EBA1u;
        } else if (i < 60) {
            f = (b & cc) | (b & d) | (cc & d);
            k = 0x8F1BBCDCu;
        } else {
            f = b ^ cc ^ d;
            k = 0xCA62C1D6u;
        }
        t = rol(a, 5) + f + e + k + w[i];
        e = d;
        d = cc;
        cc = rol(b, 30);
        b = a;
        a = t;
    }
    c->h[0] += a;
    c->h[1] += b;
    c->h[2] += cc;
    c->h[3] += d;
    c->h[4] += e;
}

static void sha1_init(sha1_t *c)
{
    static const uint32_t iv[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};

    memcpy(c->h, iv, sizeof(iv));
    c->bits = 0;
    c->used = 0;
}

static void sha1_update(sha1_t *c, const unsigned char *data, size_t len)
{
    c->bits += (uint64_t)len * 8;
    while (len > 0) {
        size_t n = 64 - c->used < len ? 64 - c->used : len;

        memcpy(c->block + c->used, data, n);
        c->used += n;
        data += n;
        len -= n;
        if (c->used == 64) {
            sha1_block(c, c->block);
            c->used = 0;
        }
    }
}

static void sha1_final(sha1_t *c, unsigned char out[20])
{
    uint64_t bits = c->bits;
    unsigned char pad = 0x80, zero = 0, lenbuf[8];
    int i;

    sha1_update(c, &pad, 1);
    while (c->used != 56) {
        sha1_update(c, &zero, 1);
    }
    for (i = 0; i < 8; i++) {
        lenbuf[i] = (unsigned char)(bits >> (56 - 8 * i));
    }
    sha1_update(c, lenbuf, 8);
    for (i = 0; i < 5; i++) {
        out[i * 4] = (unsigned char)(c->h[i] >> 24);
        out[i * 4 + 1] = (unsigned char)(c->h[i] >> 16);
        out[i * 4 + 2] = (unsigned char)(c->h[i] >> 8);
        out[i * 4 + 3] = (unsigned char)c->h[i];
    }
}

static void sha1_of(const unsigned char *data, size_t len, unsigned char out[20])
{
    sha1_t c;

    sha1_init(&c);
    sha1_update(&c, data, len);
    sha1_final(&c, out);
}

/* The MAC the Vita's package installer expects over a fake package header: SHA-1, shuffled
 * into a 64-byte block, SHA-1 again, first 16 bytes. */
static void fpkg_mac(const unsigned char *data, size_t len, unsigned char mac[MAC_SIZE])
{
    unsigned char sha[20], buf[64];

    sha1_of(data, len, sha);
    memset(buf, 0, sizeof(buf));
    memcpy(&buf[0], &sha[4], 8);
    memcpy(&buf[8], &sha[4], 8);
    memcpy(&buf[16], &sha[12], 4);
    buf[20] = sha[16];
    buf[21] = sha[1];
    buf[22] = sha[2];
    buf[23] = sha[3];
    memcpy(&buf[24], &buf[16], 8);
    sha1_of(buf, sizeof(buf), sha);
    memcpy(mac, sha, MAC_SIZE);
}

static int be32_at(const unsigned char *p, size_t size, size_t off, uint32_t *out)
{
    if (off + 4 > size) {
        return -1;
    }
    *out = ((uint32_t)p[off] << 24) | ((uint32_t)p[off + 1] << 16) | ((uint32_t)p[off + 2] << 8) | p[off + 3];
    return 0;
}

int vpk_make_head_bin(const unsigned char *tmpl, size_t tmpl_len, const char *content_id,
                      unsigned char *out)
{
    uint32_t len, off, dst;
    size_t n;

    if (tmpl_len < HDR_CONTENT_ID + VPK_CONTENT_ID_MAX) {
        return -1;
    }
    memcpy(out, tmpl, tmpl_len);
    n = strnlen(content_id, VPK_CONTENT_ID_MAX);
    memset(out + HDR_CONTENT_ID, 0, VPK_CONTENT_ID_MAX);
    memcpy(out + HDR_CONTENT_ID, content_id, n);

    if (be32_at(out, tmpl_len, HDR_MAC1_LEN, &len) != 0 || (size_t)len + MAC_SIZE > tmpl_len) {
        return -1;
    }
    fpkg_mac(out, len, out + len);

    if (be32_at(out, tmpl_len, HDR_INFO_OFF, &off) != 0 || be32_at(out, tmpl_len, HDR_INFO_LEN, &len) != 0 ||
        be32_at(out, tmpl_len, HDR_MAC2_OUT, &dst) != 0 || len < 64 || (size_t)off + len - 64 > tmpl_len ||
        (size_t)dst + MAC_SIZE > tmpl_len) {
        return -1;
    }
    fpkg_mac(out + off, len - 64, out + dst);

    if (be32_at(out, tmpl_len, HDR_MAC3_LEN, &len) != 0 || (size_t)len + MAC_SIZE > tmpl_len) {
        return -1;
    }
    fpkg_mac(out, len, out + len);
    return 0;
}
