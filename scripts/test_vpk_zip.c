/* Host test for src/vpk_zip.c; driven by scripts/test_vpk_zip.py, which builds the fixtures. */
#include "vpk_zip.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

#define CHECK(cond)                                                               \
    do {                                                                          \
        if (!(cond)) {                                                            \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                \
            failures++;                                                           \
        }                                                                         \
    } while (0)

typedef struct {
    unsigned char *data;
    size_t len;
    size_t cap;
    size_t stop_after;
} buf_t;

static int collect(void *ctx, const unsigned char *data, size_t len)
{
    buf_t *b = ctx;

    if (b->len + len > b->cap) {
        b->cap = (b->len + len) * 2;
        b->data = realloc(b->data, b->cap);
    }
    memcpy(b->data + b->len, data, len);
    b->len += len;
    return (b->stop_after != 0 && b->len >= b->stop_after) ? 1 : 0;
}

static unsigned char *slurp(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    unsigned char *p;

    if (f == NULL) {
        printf("FAIL cannot open %s\n", path);
        failures++;
        *len = 0;
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    *len = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    p = malloc(*len + 1);
    if (fread(p, 1, *len, f) != *len) {
        failures++;
    }
    fclose(f);
    return p;
}

static void path_in(char *dst, size_t n, const char *dir, const char *name)
{
    snprintf(dst, n, "%s/%s", dir, name);
}

static void expect_same(vpk_zip_t *z, const char *dir, const char *name)
{
    char p[1024];
    size_t want_len;
    unsigned char *want;
    buf_t got = {0};
    const vpk_entry_t *e = vpk_zip_find(z, name);

    CHECK(e != NULL);
    if (e == NULL) {
        return;
    }
    snprintf(p, sizeof(p), "%s/expect/%s", dir, name);
    want = slurp(p, &want_len);
    CHECK(vpk_zip_extract(z, e, collect, &got) == VPK_OK);
    CHECK(got.len == want_len);
    CHECK(want != NULL && got.len == want_len && memcmp(got.data, want, want_len) == 0);
    CHECK(e->usize == want_len);
    free(want);
    free(got.data);
}

static vpk_err_t open_fixture(vpk_zip_t **z, const char *dir, const char *name)
{
    char p[1024];

    path_in(p, sizeof(p), dir, name);
    return vpk_zip_open(z, p);
}

int main(int argc, char **argv)
{
    const char *dir;
    vpk_zip_t *z = NULL;
    const vpk_entry_t *e;
    buf_t got = {0};

    if (argc != 2) {
        fprintf(stderr, "usage: %s FIXTURE_DIR\n", argv[0]);
        return 2;
    }
    dir = argv[1];

    /* a good package: stored, deflated, a directory entry and a multi-chunk entry */
    CHECK(open_fixture(&z, dir, "good.zip") == VPK_OK);
    if (z != NULL) {
        CHECK(vpk_zip_count(z) == 5);
        expect_same(z, dir, "eboot.bin");
        expect_same(z, dir, "sce_sys/param.sfo");
        expect_same(z, dir, "big.bin");
        e = vpk_zip_find(z, "sce_sys/param.sfo");
        CHECK(e != NULL && e->method == 0 && !e->is_dir);
        e = vpk_zip_find(z, "big.bin");
        CHECK(e != NULL && e->method == 8 && e->csize < e->usize);
        e = vpk_zip_find(z, "sce_sys/");
        CHECK(e != NULL && e->is_dir);
        CHECK(vpk_zip_find(z, "nope") == NULL);
        e = vpk_zip_find(z, "big.bin");
        got.stop_after = 1;
        CHECK(e != NULL && vpk_zip_extract(z, e, collect, &got) == VPK_E_SINK);
        vpk_zip_close(z);
    }

    CHECK(open_fixture(&z, dir, "bad_crc.zip") == VPK_OK);
    if (z != NULL) {
        buf_t b = {0};

        e = vpk_zip_find(z, "eboot.bin");
        CHECK(e != NULL && vpk_zip_extract(z, e, collect, &b) == VPK_E_CRC);
        free(b.data);
        vpk_zip_close(z);
    }

    CHECK(open_fixture(&z, dir, "truncated.zip") == VPK_E_NOT_ZIP);
    CHECK(open_fixture(&z, dir, "not_a_zip.bin") == VPK_E_NOT_ZIP);
    CHECK(open_fixture(&z, dir, "missing.zip") == VPK_E_IO);
    CHECK(open_fixture(&z, dir, "name_dotdot.zip") == VPK_E_BAD_NAME);
    CHECK(open_fixture(&z, dir, "name_abs.zip") == VPK_E_BAD_NAME);
    CHECK(open_fixture(&z, dir, "name_backslash.zip") == VPK_E_BAD_NAME);
    CHECK(open_fixture(&z, dir, "name_device.zip") == VPK_E_BAD_NAME);
    CHECK(open_fixture(&z, dir, "name_long.zip") == VPK_E_BAD_NAME);
    CHECK(open_fixture(&z, dir, "encrypted.zip") == VPK_E_UNSUPPORTED);
    CHECK(open_fixture(&z, dir, "bzip2.zip") == VPK_E_UNSUPPORTED);
    CHECK(open_fixture(&z, dir, "oob_offset.zip") == VPK_E_CORRUPT);

    free(got.data);
    if (failures == 0) {
        printf("ok\n");
    }
    return failures == 0 ? 0 : 1;
}
