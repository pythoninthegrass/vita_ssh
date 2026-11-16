/*
 * libssh's stock SFTP server over the Vita path layer. See sftp_server.h.
 *
 * Two things the stock code does not do for us:
 *
 *   - It assumes each channel data chunk holds whole SFTP packets. A 32 KB write
 *     is larger than one SSH data packet, so it arrives in pieces and the stock
 *     callback rejects it. sh_sftp_data() reassembles first and hands over one
 *     complete packet at a time.
 *   - It calls libc with the client's POSIX path. The __wrap_ functions below
 *     (linked in with -Wl,--wrap) translate through sh_path.c first.
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include <libssh/sftp.h>
#include <libssh/sftpserver.h>

#include "sftp_server.h"
#include "sshd.h"

/* OpenSSH's own ceiling for one SFTP packet. */
#define SFTP_MAX_PACKET (256 * 1024)
#define SFTP_MAX_FILES 32
#define SFTP_MAX_DIRS 16
#define PART_SUFFIX ".part"

typedef struct {
    int fd;           /* -1 when the slot is free */
    char *part;       /* sce path being written, NULL for a direct open */
    char *target;     /* sce path the part file is renamed to */
} sftp_file_t;

typedef struct {
    DIR *real;        /* NULL for the synthetic root listing */
    int used;
    size_t next;      /* root listing: next device index */
    struct dirent ent;
} sftp_dir_t;

struct sh_sftp {
    sftp_session sftp;
    uint8_t *buf;
    size_t len;
    size_t cap;
    sftp_file_t files[SFTP_MAX_FILES];
    sftp_dir_t dirs[SFTP_MAX_DIRS];
};

/* Set only while the stock data callback runs. The server is single-threaded,
 * so a pair of globals is enough and nothing else needs to see them. */
static sh_sftp_t *active;
static const sh_fsctx_t *active_fs;

int __real_open(const char *path, int flags, ...);
int __real_close(int fd);
DIR *__real_opendir(const char *path);
struct dirent *__real_readdir(DIR *dir);
int __real_closedir(DIR *dir);
int __real_stat(const char *path, struct stat *st);
int __real_lstat(const char *path, struct stat *st);
int __real_mkdir(const char *path, mode_t mode);
int __real_rmdir(const char *path);
int __real_unlink(const char *path);
int __real_chmod(const char *path, mode_t mode);
int __real_utimes(const char *path, const struct timeval tv[2]);

/* ------------------------------------------------------------------ */
/* path translation                                                   */
/* ------------------------------------------------------------------ */

static int resolve(const char *path, sh_target_t *t)
{
    return sh_path_resolve(active_fs, path, t);
}

/* The mutating calls refuse before the syscall, same as the shell does, so a
 * write to /os0 says "read-only file system" and not an sceIo code. */
static int writable_target(const char *path, sh_target_t *t)
{
    if (resolve(path, t) != 0) {
        return -1;
    }
    if (t->kind == SH_TARGET_ROOT || t->dev == NULL || !t->dev->writable) {
        errno = EROFS;
        return -1;
    }
    return 0;
}

static void synth_dir_stat(struct stat *st, int writable)
{
    memset(st, 0, sizeof(*st));
    st->st_mode = S_IFDIR | (writable ? 0755 : 0555);
}

static int stat_common(const char *path, struct stat *st, int follow)
{
    sh_target_t t;
    int rc;

    if (resolve(path, &t) != 0) {
        return -1;
    }
    if (t.kind == SH_TARGET_ROOT) {
        synth_dir_stat(st, 0);
        return 0;
    }
    rc = follow ? __real_stat(t.sce, st) : __real_lstat(t.sce, st);
    if (rc != 0 && t.kind == SH_TARGET_DEVICE && sh_device_present(t.dev)) {
        /* sceIo cannot stat a bare "ux0:", but the device is a directory. */
        synth_dir_stat(st, t.dev->writable);
        return 0;
    }
    return rc;
}

/* ------------------------------------------------------------------ */
/* wrapped libc                                                       */
/* ------------------------------------------------------------------ */

static sftp_file_t *file_slot(int fd)
{
    int i;

    for (i = 0; i < SFTP_MAX_FILES; i++) {
        if (active->files[i].fd == fd) {
            return &active->files[i];
        }
    }
    return NULL;
}

int __wrap_open(const char *path, int flags, ...)
{
    mode_t mode = 0;
    sh_target_t t;
    sftp_file_t *slot;
    char *part = NULL;
    char *target = NULL;
    int fd;

    if (flags & O_CREAT) {
        va_list ap;

        va_start(ap, flags);
        mode = (mode_t)va_arg(ap, int);
        va_end(ap);
    }
    if (active == NULL) {
        return __real_open(path, flags, mode);
    }
    if ((flags & (O_WRONLY | O_RDWR | O_CREAT | O_TRUNC)) != 0) {
        if (writable_target(path, &t) != 0) {
            return -1;
        }
    } else if (resolve(path, &t) != 0) {
        return -1;
    }
    if (t.kind == SH_TARGET_ROOT) {
        errno = EISDIR;
        return -1;
    }
    slot = file_slot(-1);
    if (slot == NULL) {
        errno = EMFILE;
        return -1;
    }
    if ((flags & O_CREAT) && (flags & O_TRUNC)) {
        size_t len = strlen(t.sce);

        part = malloc(len + sizeof(PART_SUFFIX));
        target = strdup(t.sce);
        if (part == NULL || target == NULL) {
            free(part);
            free(target);
            errno = ENOMEM;
            return -1;
        }
        memcpy(part, t.sce, len);
        memcpy(part + len, PART_SUFFIX, sizeof(PART_SUFFIX));
        fd = __real_open(part, flags, mode);
    } else {
        fd = __real_open(t.sce, flags, mode);
    }
    if (fd < 0) {
        int saved = errno;

        free(part);
        free(target);
        errno = saved;
        return -1;
    }
    slot->fd = fd;
    slot->part = part;
    slot->target = target;
    return fd;
}

static void slot_release(sftp_file_t *slot)
{
    free(slot->part);
    free(slot->target);
    slot->part = NULL;
    slot->target = NULL;
    slot->fd = -1;
}

int __wrap_close(int fd)
{
    sftp_file_t *slot = active != NULL ? file_slot(fd) : NULL;
    int rc;

    if (slot == NULL) {
        return __real_close(fd);
    }
    rc = __real_close(fd);
    if (slot->part != NULL) {
        if (rc == 0) {
            /* sceIoRename will not replace an existing file. */
            if (rename(slot->part, slot->target) != 0 &&
                (errno != EEXIST || __real_unlink(slot->target) != 0 ||
                 rename(slot->part, slot->target) != 0)) {
                int saved = errno;

                sh_console_log("sftp: rename %s -> %s failed: %s", slot->part,
                               slot->target, strerror(saved));
                __real_unlink(slot->part);
                rc = -1;
                errno = saved;
            }
        } else {
            __real_unlink(slot->part);
        }
    }
    slot_release(slot);
    return rc;
}

static sftp_dir_t *dir_slot_for(DIR *dir)
{
    int i;

    for (i = 0; i < SFTP_MAX_DIRS; i++) {
        sftp_dir_t *d = &active->dirs[i];

        if (d->used && ((void *)d == (void *)dir || (d->real != NULL && d->real == dir))) {
            return d;
        }
    }
    return NULL;
}

DIR *__wrap_opendir(const char *path)
{
    sh_target_t t;
    sftp_dir_t *slot = NULL;
    int i;

    if (active == NULL) {
        return __real_opendir(path);
    }
    if (resolve(path, &t) != 0) {
        return NULL;
    }
    for (i = 0; i < SFTP_MAX_DIRS; i++) {
        if (!active->dirs[i].used) {
            slot = &active->dirs[i];
            break;
        }
    }
    if (slot == NULL) {
        errno = EMFILE;
        return NULL;
    }
    memset(slot, 0, sizeof(*slot));
    if (t.kind == SH_TARGET_ROOT) {
        slot->used = 1;
        return (DIR *)slot;
    }
    slot->real = __real_opendir(t.sce);
    if (slot->real == NULL) {
        return NULL;
    }
    slot->used = 1;
    return slot->real;
}

struct dirent *__wrap_readdir(DIR *dir)
{
    sftp_dir_t *slot = active != NULL ? dir_slot_for(dir) : NULL;
    size_t count;
    const sh_device_t *devs;

    if (slot == NULL || slot->real != NULL) {
        return __real_readdir(dir);
    }
    devs = sh_devices(&count);
    while (slot->next < count) {
        const sh_device_t *dev = &devs[slot->next++];

        if (!sh_device_present(dev)) {
            continue;
        }
        memset(&slot->ent, 0, sizeof(slot->ent));
        /* posix is "/ux0": the entry name is what follows the slash. */
        snprintf(slot->ent.d_name, sizeof(slot->ent.d_name), "%s",
                 dev->posix + 1);
        return &slot->ent;
    }
    return NULL;
}

int __wrap_closedir(DIR *dir)
{
    sftp_dir_t *slot = active != NULL ? dir_slot_for(dir) : NULL;
    int rc = 0;

    if (slot == NULL) {
        return __real_closedir(dir);
    }
    if (slot->real != NULL) {
        rc = __real_closedir(slot->real);
    }
    memset(slot, 0, sizeof(*slot));
    return rc;
}

int __wrap_stat(const char *path, struct stat *st)
{
    return active == NULL ? __real_stat(path, st) : stat_common(path, st, 1);
}

int __wrap_lstat(const char *path, struct stat *st)
{
    return active == NULL ? __real_lstat(path, st) : stat_common(path, st, 0);
}

int __wrap_mkdir(const char *path, mode_t mode)
{
    sh_target_t t;

    if (active == NULL) {
        return __real_mkdir(path, mode);
    }
    if (writable_target(path, &t) != 0) {
        return -1;
    }
    return __real_mkdir(t.sce, mode);
}

int __wrap_rmdir(const char *path)
{
    sh_target_t t;

    if (active == NULL) {
        return __real_rmdir(path);
    }
    if (writable_target(path, &t) != 0) {
        return -1;
    }
    return __real_rmdir(t.sce);
}

int __wrap_unlink(const char *path)
{
    sh_target_t t;

    if (active == NULL) {
        return __real_unlink(path);
    }
    if (writable_target(path, &t) != 0) {
        return -1;
    }
    return __real_unlink(t.sce);
}

int __wrap_chmod(const char *path, mode_t mode)
{
    sh_target_t t;

    if (active == NULL) {
        return __real_chmod(path, mode);
    }
    if (writable_target(path, &t) != 0) {
        return -1;
    }
    return __real_chmod(t.sce, mode);
}

int __wrap_utimes(const char *path, const struct timeval tv[2])
{
    sh_target_t t;

    if (active == NULL) {
        return __real_utimes(path, tv);
    }
    if (writable_target(path, &t) != 0) {
        return -1;
    }
    return __real_utimes(t.sce, tv);
}

/* Canonicalization is purely lexical: the Vita has no symlinks to follow, and
 * clients realpath a name they are about to create. */
char *__wrap_realpath(const char *path, char *resolved)
{
    sh_target_t t;

    if (active == NULL) {
        errno = ENOSYS;
        return NULL;
    }
    if (resolve(path, &t) != 0) {
        return NULL;
    }
    if (resolved != NULL) {
        snprintf(resolved, SH_POSIX_PATH_MAX, "%s", t.posix);
        return resolved;
    }
    return strdup(t.posix);
}

/* ------------------------------------------------------------------ */
/* session                                                            */
/* ------------------------------------------------------------------ */

sh_sftp_t *sh_sftp_new(ssh_session session, ssh_channel channel)
{
    sh_sftp_t *s = calloc(1, sizeof(*s));
    int i;

    if (s == NULL) {
        return NULL;
    }
    for (i = 0; i < SFTP_MAX_FILES; i++) {
        s->files[i].fd = -1;
    }
    if (sftp_channel_default_subsystem_request(session, channel, "sftp",
                                               &s->sftp) != SSH_OK) {
        free(s);
        return NULL;
    }
    return s;
}

static int feed_packet(sh_sftp_t *s, const sh_fsctx_t *fs, void *packet,
                       uint32_t len)
{
    int rc;

    active = s;
    active_fs = fs;
    rc = sftp_channel_default_data_callback(NULL, NULL, packet, len, 0,
                                            &s->sftp);
    active = NULL;
    active_fs = NULL;
    return rc < 0 ? -1 : 0;
}

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

int sh_sftp_data(sh_sftp_t *s, const sh_fsctx_t *fs, const void *data,
                 uint32_t len)
{
    if (s == NULL) {
        return -1;
    }
    if (s->len + len > s->cap) {
        size_t cap = s->len + len;
        uint8_t *grown;

        if (cap > 2 * SFTP_MAX_PACKET) {
            return -1;
        }
        grown = realloc(s->buf, cap);
        if (grown == NULL) {
            return -1;
        }
        s->buf = grown;
        s->cap = cap;
    }
    memcpy(s->buf + s->len, data, len);
    s->len += len;

    while (s->len >= sizeof(uint32_t)) {
        uint32_t body = be32(s->buf);
        size_t total;

        if (body < 1 || body > SFTP_MAX_PACKET) {
            return -1;
        }
        total = (size_t)body + sizeof(uint32_t);
        if (s->len < total) {
            break;
        }
        if (feed_packet(s, fs, s->buf, (uint32_t)total) != 0) {
            return -1;
        }
        memmove(s->buf, s->buf + total, s->len - total);
        s->len -= total;
    }
    return 0;
}

void sh_sftp_free(sh_sftp_t *s)
{
    int i;

    if (s == NULL) {
        return;
    }
    for (i = 0; i < SFTP_MAX_FILES; i++) {
        sftp_file_t *f = &s->files[i];

        if (f->fd < 0) {
            continue;
        }
        __real_close(f->fd);
        if (f->part != NULL) {
            __real_unlink(f->part);
        }
        slot_release(f);
    }
    for (i = 0; i < SFTP_MAX_DIRS; i++) {
        if (s->dirs[i].used && s->dirs[i].real != NULL) {
            __real_closedir(s->dirs[i].real);
        }
    }
    sftp_server_free(s->sftp);
    free(s->buf);
    free(s);
}
