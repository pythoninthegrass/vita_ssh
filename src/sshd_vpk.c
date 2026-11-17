/*
 * `vpk list` and `vpk install`: install a VPK of this app through the Vita's own package
 * installer, the way VitaShell does.
 *
 * ux0:app/<TITLEID> is read-only to the app even when built UNSAFE, and a copy dropped into
 * ux0:patch/<TITLEID> is ignored at launch (both verified on a device), so the files cannot
 * be laid down by hand. Instead the package is unpacked and CRC-checked into its private stage,
 * given a generated sce_sys/package/head.bin, and handed to scePromoterUtilityPromotePkgWithRif,
 * which registers and installs it. Any failure before that call leaves the installed app
 * untouched; the installer's own failure cannot be rolled back from here.
 *
 * Only this app's own title ID is accepted (SSHD_TITLEID, passed in from CMakeLists.txt).
 * A bundled second title promotes the server after it exits, then relaunches it.
 */
#include "sshd.h"
#include "vpk_head.h"
#include "vpk_zip.h"
#include "vpk_update.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <psp2/io/devctl.h>
#include <psp2/appmgr.h>
#include <psp2/kernel/processmgr.h>

#ifndef SSHD_TITLEID
#error "SSHD_TITLEID must be defined by CMakeLists.txt"
#endif

#define DEVCTL_GET_DEV_INFO 0x3001

#define HEAD_BIN_REL "sce_sys/package/head.bin"
static int update_pending;

#define SFO_MAX 65536
#define EBOOT_MIN_SIZE 4096
#define RM_MAX_DEPTH 16
#define PATH_MAX_ENTRY (SH_SCE_PATH_MAX)

typedef struct {
    sh_session_t *s;
    int fd;
    unsigned char head[4];
    size_t seen;
} file_sink_t;

typedef struct {
    unsigned char *data;
    size_t len;
} mem_sink_t;

/* ------------------------------------------------------------------ */
/* package inspection                                                   */
/* ------------------------------------------------------------------ */

static int mem_write(void *ctx, const unsigned char *data, size_t len)
{
    mem_sink_t *m = ctx;

    if (m->len + len > SFO_MAX) {
        return 1;
    }
    memcpy(m->data + m->len, data, len);
    m->len += len;
    return 0;
}

static uint32_t le32(const unsigned char *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static unsigned le16(const unsigned char *p)
{
    return (unsigned)(p[0] | (p[1] << 8));
}

/* Reads one string value out of a param.sfo image: header, then 16-byte index entries pointing
 * into a key table and a data table. Returns 0 and fills out, or -1. */
static int sfo_string(const unsigned char *sfo, size_t len, const char *key, char *out, size_t outlen)
{
    uint32_t key_off, data_off, count, i;

    if (len < 20 || memcmp(sfo, "\0PSF", 4) != 0) {
        return -1;
    }
    key_off = le32(sfo + 8);
    data_off = le32(sfo + 12);
    count = le32(sfo + 16);
    for (i = 0; i < count && 20 + (size_t)(i + 1) * 16 <= len; i++) {
        const unsigned char *ent = sfo + 20 + (size_t)i * 16;
        size_t k = (size_t)key_off + le16(ent);
        size_t d = (size_t)data_off + le32(ent + 12);
        size_t dlen = le32(ent + 4);

        if (k >= len || strncmp((const char *)sfo + k, key, len - k) != 0) {
            continue;
        }
        if (d + dlen > len || dlen == 0 || dlen >= outlen) {
            return -1;
        }
        memcpy(out, sfo + d, dlen);
        out[dlen] = '\0';
        out[strnlen(out, dlen)] = '\0';
        return 0;
    }
    return -1;
}

typedef struct {
    vpk_zip_t *zip;
    char title_id[16];
    char content_id[VPK_CONTENT_ID_MAX + 1];
    const vpk_entry_t *eboot;
    long long total;
    size_t files;
} package_t;

static void package_close(package_t *p)
{
    vpk_zip_close(p->zip);
    p->zip = NULL;
}

/* Opens the package and reads its title ID; never writes. Reports its own errors. */
static int package_open(sh_session_t *s, const char *sce, const char *name, package_t *p)
{
    mem_sink_t m;
    const vpk_entry_t *sfo;
    vpk_err_t err;
    size_t i;

    memset(p, 0, sizeof(*p));
    err = vpk_zip_open(&p->zip, sce);
    if (err != VPK_OK) {
        sh_eprintf(s, "vpk: %s: %s\n", name, vpk_zip_strerror(err));
        return -1;
    }
    for (i = 0; i < vpk_zip_count(p->zip); i++) {
        const vpk_entry_t *e = vpk_zip_entry(p->zip, i);

        if (!e->is_dir) {
            p->total += e->usize;
            p->files++;
        }
    }
    p->eboot = vpk_zip_find(p->zip, "eboot.bin");
    sfo = vpk_zip_find(p->zip, "sce_sys/param.sfo");
    if (sfo == NULL || sfo->usize > SFO_MAX) {
        sh_eprintf(s, "vpk: %s: no sce_sys/param.sfo, not a VPK\n", name);
        goto fail;
    }
    m.data = malloc(SFO_MAX);
    m.len = 0;
    if (m.data == NULL) {
        sh_eprintf(s, "vpk: out of memory\n");
        goto fail;
    }
    err = vpk_zip_extract(p->zip, sfo, mem_write, &m);
    if (err != VPK_OK || sfo_string(m.data, m.len, "TITLE_ID", p->title_id, sizeof(p->title_id)) != 0) {
        sh_eprintf(s, "vpk: %s: cannot read the title ID from param.sfo\n", name);
        free(m.data);
        goto fail;
    }
    /* CONTENT_ID is empty in a vita-mksfoex param.sfo: head.bin then takes a name made from the title ID */
    if (sfo_string(m.data, m.len, "CONTENT_ID", p->content_id, sizeof(p->content_id)) != 0 ||
        p->content_id[0] == '\0') {
        snprintf(p->content_id, sizeof(p->content_id), "EP9000-%s_00-0000000000000000", p->title_id);
    }
    free(m.data);
    return 0;

fail:
    package_close(p);
    return -1;
}

/* Refuses a package that is not this app's, or whose eboot.bin cannot be one. */
static int package_check(sh_session_t *s, const char *name, const package_t *p)
{
    if (strcmp(p->title_id, SSHD_TITLEID) != 0) {
        sh_eprintf(s, "vpk: %s: title ID is %s, this app is %s\n", name, p->title_id, SSHD_TITLEID);
        return -1;
    }
    if (p->eboot == NULL) {
        sh_eprintf(s, "vpk: %s: no eboot.bin in package\n", name);
        return -1;
    }
    if (p->eboot->usize < EBOOT_MIN_SIZE) {
        sh_eprintf(s, "vpk: %s: eboot.bin is %u bytes, too small to be an executable\n", name,
                   (unsigned)p->eboot->usize);
        return -1;
    }
    return 0;
}

int sh_vpk_list(sh_session_t *s, const char *sce, const char *name)
{
    package_t p;
    size_t i;

    if (package_open(s, sce, name, &p) != 0) {
        return 1;
    }
    sh_printf(s, "Title ID: %s%s\n", p.title_id,
              strcmp(p.title_id, SSHD_TITLEID) == 0 ? " (this app)" : " (not this app)");
    for (i = 0; i < vpk_zip_count(p.zip); i++) {
        const vpk_entry_t *e = vpk_zip_entry(p.zip, i);

        if (!e->is_dir) {
            sh_printf(s, "%10u  %s\n", (unsigned)e->usize, e->name);
        }
    }
    sh_printf(s, "%u files, %lld bytes\n", (unsigned)p.files, p.total);
    package_close(&p);
    return 0;
}

/* ------------------------------------------------------------------ */
/* filesystem helpers                                                   */
/* ------------------------------------------------------------------ */

static int join_path(char *dst, size_t n, const char *base, const char *rel)
{
    int w = snprintf(dst, n, "%s/%s", base, rel);

    if (w < 0 || (size_t)w >= n) {
        errno = ENAMETOOLONG;
        return -1;
    }
    return 0;
}

/* Creates every directory above the last component of base/rel. */
static int make_parents(const char *base, const char *rel)
{
    char path[PATH_MAX_ENTRY];
    size_t i;

    if (join_path(path, sizeof(path), base, rel) != 0) {
        return -1;
    }
    for (i = strlen(base) + 1; path[i] != '\0'; i++) {
        if (path[i] == '/') {
            path[i] = '\0';
            if (mkdir(path, 0755) != 0 && errno != EEXIST) {
                return -1;
            }
            path[i] = '/';
        }
    }
    return 0;
}

static int remove_tree(const char *path, int depth)
{
    struct stat top;
    DIR *dir;
    struct dirent *ent;
    int rc = 0;

    /* opendir() on a missing path answers ENOTDIR here, not ENOENT, so absence is asked of stat() */
    if (stat(path, &top) != 0) {
        return 0;
    }
    dir = opendir(path);
    if (dir == NULL) {
        return -1;
    }
    while ((ent = readdir(dir)) != NULL) {
        char child[PATH_MAX_ENTRY];
        struct stat st;

        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
            continue;
        }
        if (join_path(child, sizeof(child), path, ent->d_name) != 0 || stat(child, &st) != 0) {
            rc = -1;
            continue;
        }
        if (S_ISDIR(st.st_mode)) {
            if (depth >= RM_MAX_DEPTH || remove_tree(child, depth + 1) != 0) {
                rc = -1;
            }
        } else if (unlink(child) != 0) {
            rc = -1;
        }
    }
    closedir(dir);
    if (rmdir(path) != 0) {
        rc = -1;
    }
    return rc;
}

static long long free_space(const char *dev)
{
    SceIoDevInfo info;

    memset(&info, 0, sizeof(info));
    if (sceIoDevctl(dev, DEVCTL_GET_DEV_INFO, NULL, 0, &info, sizeof(info)) < 0) {
        return -1;
    }
    return (long long)info.free_size;
}

static int write_all(int fd, const unsigned char *data, size_t len)
{
    size_t put = 0;

    while (put < len) {
        ssize_t one = write(fd, data + put, len - put);

        if (one <= 0) {
            return -1;
        }
        put += (size_t)one;
    }
    return 0;
}

static int file_write(void *ctx, const unsigned char *data, size_t len)
{
    file_sink_t *f = ctx;

    if (sh_session_interrupted(f->s)) {
        errno = EINTR;
        return 1;
    }
    if (f->seen < sizeof(f->head)) {
        size_t n = sizeof(f->head) - f->seen;

        memcpy(f->head + f->seen, data, n < len ? n : len);
    }
    f->seen += len;
    return write_all(f->fd, data, len) != 0;
}

/* ------------------------------------------------------------------ */
/* stage and promote                                                    */
/* ------------------------------------------------------------------ */

static int stage_entry(sh_session_t *s, package_t *p, const vpk_entry_t *e, const char *base)
{
    char path[PATH_MAX_ENTRY];
    file_sink_t f;
    vpk_err_t err;
    int saved;

    if (make_parents(base, e->name) != 0 || join_path(path, sizeof(path), base, e->name) != 0) {
        sh_eprintf(s, "vpk: %s: %s\n", e->name, strerror(errno));
        return -1;
    }
    memset(&f, 0, sizeof(f));
    f.s = s;
    f.fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (f.fd < 0) {
        sh_eprintf(s, "vpk: %s: %s\n", path, strerror(errno));
        return -1;
    }
    err = vpk_zip_extract(p->zip, e, file_write, &f);
    saved = errno;
    if (close(f.fd) != 0 && err == VPK_OK) {
        err = VPK_E_SINK;
        saved = errno;
    }
    if (err == VPK_E_SINK) {
        sh_eprintf(s, "vpk: %s: %s\n", e->name, saved == EINTR ? "interrupted" : strerror(saved));
        return -1;
    }
    if (err != VPK_OK) {
        sh_eprintf(s, "vpk: %s: %s\n", e->name, vpk_zip_strerror(err));
        return -1;
    }
    if (e == p->eboot && (f.seen < 4 || memcmp(f.head, "SCE\0", 4) != 0)) {
        sh_eprintf(s, "vpk: eboot.bin does not start with the SCE magic, refusing\n");
        return -1;
    }
    return 0;
}

static int stage_package(sh_session_t *s, package_t *p, const char *base)
{
    size_t i;

    if (remove_tree(base, 0) != 0) {
        sh_eprintf(s, "vpk: %s: %s\n", base, strerror(errno));
        return -1;
    }
    if (mkdir(base, 0755) != 0) {
        sh_eprintf(s, "vpk: %s: %s\n", base, strerror(errno));
        return -1;
    }
    for (i = 0; i < vpk_zip_count(p->zip); i++) {
        const vpk_entry_t *e = vpk_zip_entry(p->zip, i);

        if (!e->is_dir && stage_entry(s, p, e, base) != 0) {
            return -1;
        }
    }
    return 0;
}

extern const unsigned char vpk_head_template[];
extern const unsigned long vpk_head_template_len;

static int write_head_bin(sh_session_t *s, const package_t *p, const char *base)
{
    unsigned char *head = malloc(vpk_head_template_len);
    char path[PATH_MAX_ENTRY];
    int fd, rc = -1;

    if (head == NULL) {
        sh_eprintf(s, "vpk: out of memory\n");
        return -1;
    }
    if (vpk_make_head_bin(vpk_head_template, vpk_head_template_len, p->content_id, head) != 0) {
        sh_eprintf(s, "vpk: head.bin template is malformed\n");
        goto out;
    }
    if (make_parents(base, HEAD_BIN_REL) != 0 || join_path(path, sizeof(path), base, HEAD_BIN_REL) != 0) {
        sh_eprintf(s, "vpk: %s: %s\n", HEAD_BIN_REL, strerror(errno));
        goto out;
    }
    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0 || write_all(fd, head, vpk_head_template_len) != 0) {
        sh_eprintf(s, "vpk: %s: %s\n", path, strerror(errno));
        if (fd >= 0) {
            close(fd);
        }
        goto out;
    }
    if (close(fd) != 0) {
        sh_eprintf(s, "vpk: %s: %s\n", path, strerror(errno));
        goto out;
    }
    rc = 0;
out:
    free(head);
    return rc;
}

/* Called only after the command's output, status and EOF have been drained. */
void sh_vpk_handoff(void)
{
    int res;

    if (!update_pending) {
        return;
    }
    update_pending = 0;
    res = sceAppMgrLaunchAppByUri(0xFFFFF, "psgm:play?titleid=" SSHD_UPDATER_TITLEID);
    if (res < 0) {
        sh_console_log("vpk: launch updater failed: 0x%08X", (unsigned)res);
        unlink(VPK_UPDATE_READY);
        return;
    }
    sceKernelExitProcess(0);
}

int sh_vpk_pending(void)
{
    return update_pending;
}

int sh_vpk_install(sh_session_t *s, const char *sce, const char *name)
{
    package_t p, helper;
    long long avail;
    const char *stage = "";
    int res, rc = 1;
    FILE *ready;

    if (package_open(s, sce, name, &p) != 0) {
        return 1;
    }
    if (package_check(s, name, &p) != 0) {
        goto out;
    }
    /* Use the currently running app's trusted helper, not a file from the uploaded package. */
    if (package_open(s, "app0:updater.vpk", "bundled updater", &helper) != 0) {
        goto out;
    }
    if (strcmp(helper.title_id, SSHD_UPDATER_TITLEID) != 0 ||
        helper.eboot == NULL || helper.eboot->usize < EBOOT_MIN_SIZE) {
        sh_eprintf(s, "vpk: invalid bundled updater\n");
        package_close(&helper);
        goto out;
    }
    avail = free_space("ux0:");
    /* Stage plus an installed copy, with slack for metadata. */
    if (avail >= 0 && avail < 2 * (p.total + helper.total) + 1024 * 1024) {
        sh_eprintf(s, "vpk: not enough space on ux0 for staging and installation\n");
        goto helper_out;
    }
    if (mkdir(VPK_UPDATE_ROOT, 0755) != 0 && errno != EEXIST) {
        sh_eprintf(s, "vpk: %s: %s\n", VPK_UPDATE_ROOT, strerror(errno));
        goto helper_out;
    }
    unlink(VPK_UPDATE_READY);
    sh_printf(s, "Unpacking %u files (%lld bytes)...\n", (unsigned)p.files, p.total);
    if (stage_package(s, &p, VPK_UPDATE_PKG) != 0 ||
        write_head_bin(s, &p, VPK_UPDATE_PKG) != 0 ||
        stage_package(s, &helper, VPK_UPDATE_HELPER) != 0 ||
        write_head_bin(s, &helper, VPK_UPDATE_HELPER) != 0) {
        sh_eprintf(s, "vpk: install aborted, installed app unchanged\n");
        goto cleanup;
    }
    sh_printf(s, "Installing the updater...\n");
    res = vpk_promote(VPK_UPDATE_HELPER, &stage);
    if (res < 0) {
        sh_eprintf(s, "vpk: the system installer failed at \"%s\": 0x%08X\n", stage, (unsigned)res);
        goto cleanup;
    }
    ready = fopen(VPK_UPDATE_READY, "wb");
    if (ready == NULL) {
        sh_eprintf(s, "vpk: cannot write updater handoff: %s\n", strerror(errno));
        goto cleanup;
    }
    res = fwrite(SSHD_TITLEID, 1, strlen(SSHD_TITLEID), ready) == strlen(SSHD_TITLEID);
    if (fclose(ready) != 0) {
        res = 0;
    }
    if (!res) {
        sh_eprintf(s, "vpk: cannot finish updater handoff\n");
        unlink(VPK_UPDATE_READY);
        goto cleanup;
    }
    sh_printf(s, "Staged. Switching to updater; SSH will disconnect. Reconnect after the server relaunches.\n"
                 "Result: /ux0/data/vitasshd-update/result.txt\n");
    update_pending = 1;
    rc = 0;

cleanup:
    if (!update_pending) {
        remove_tree(VPK_UPDATE_PKG, 0);
    }
    remove_tree(VPK_UPDATE_HELPER, 0);
helper_out:
    package_close(&helper);
out:
    package_close(&p);
    return rc;
}
