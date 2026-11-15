/*
 * POSIX path <-> Vita device path. See include/sh_path.h for the rules this
 * file is the only place allowed to implement.
 */
#include "sh_path.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <psp2/io/stat.h>

/*
 * VitaShell's list, plus app0:. VitaShell omits app0: because it *is* the app
 * running there; we are a different app, so listing the server's own partition
 * is useful. sa0:, sd0:, ud0:, vd0:, host0:, imc0:, xmc0: and uma0: exist only
 * while something is mounted in them, hence the removable flag: they are
 * probed instead of always being listed.
 *
 * Writable is deliberately permissive. A device marked writable that turns out
 * to reject the write produces a normal errno from the syscall; a device marked
 * read-only that could have been written produces a lie, and a lie the user
 * cannot work around is worse than an error they can read.
 */
static const sh_device_t devices[] = {
    {"gro0:", "/gro0", 0, 0},
    {"grw0:", "/grw0", 1, 0},
    {"imc0:", "/imc0", 1, 1},
    {"os0:", "/os0", 0, 0},
    {"pd0:", "/pd0", 1, 0},
    {"sa0:", "/sa0", 1, 1},
    {"sd0:", "/sd0", 0, 1},
    {"tm0:", "/tm0", 1, 0},
    {"ud0:", "/ud0", 0, 1},
    {"uma0:", "/uma0", 1, 1},
    {"ur0:", "/ur0", 1, 0},
    {"ux0:", "/ux0", 1, 0},
    {"vd0:", "/vd0", 1, 1},
    {"vs0:", "/vs0", 1, 0},
    {"xmc0:", "/xmc0", 1, 1},
    {"host0:", "/host0", 1, 1},
    {"app0:", "/app0", 0, 0},
};

#define N_DEVICES (sizeof(devices) / sizeof(devices[0]))

/* Depth cap. Anything deeper is either a mistake or an attempt to make the
 * shell spend its stack. */
#define SH_MAX_DEPTH 64

const sh_device_t *sh_devices(size_t *count)
{
    if (count != NULL) {
        *count = N_DEVICES;
    }
    return devices;
}

const sh_device_t *sh_device_by_posix(const char *posix, size_t len)
{
    size_t i;

    if (posix == NULL || len == 0) {
        return NULL;
    }
    /* Accept a component with or without its leading slash: the normalizer
     * hands out bare components, callers holding a whole path have the slash. */
    if (posix[0] == '/') {
        posix++;
        len--;
        if (len == 0) {
            return NULL;
        }
    }
    for (i = 0; i < N_DEVICES; i++) {
        size_t dl = strlen(devices[i].posix) - 1; /* the name without its slash */

        if (dl == len && strncmp(devices[i].posix + 1, posix, len) == 0) {
            return &devices[i];
        }
    }
    return NULL;
}

int sh_device_present(const sh_device_t *dev)
{
    SceIoStat st;

    if (dev == NULL) {
        return 0;
    }
    if (!dev->removable) {
        return 1; /* always there; probing it would prove nothing */
    }
    return sceIoGetstat(dev->sce, &st) >= 0;
}

/*
 * Splits raw on '/', dropping "." and empty components and applying ".."
 * against the component stack. Components are written into buf NUL-separated
 * and reported back as pointers into it.
 *
 * The stack is the whole point: ".." pops but never below zero, so a relative
 * path anchored on a device cannot end up on another one, and "/" stays put.
 */
static int normalize(const char *raw, char *buf, size_t buflen,
                     const char **comp, size_t *ncomp_out)
{
    const char *stack[SH_MAX_DEPTH];
    size_t r = 0, w = 0, n = 0;

    for (;;) {
        size_t start, len;

        while (raw[r] == '/') {
            r++;
        }
        if (raw[r] == '\0') {
            break;
        }
        start = r;
        while (raw[r] != '\0' && raw[r] != '/') {
            r++;
        }
        len = (size_t)(r - start);

        if (len == 1 && raw[start] == '.') {
            continue;
        }
        if (len == 2 && raw[start] == '.' && raw[start + 1] == '.') {
            if (n > 0) {
                n--;
            }
            continue;
        }
        if (n >= SH_MAX_DEPTH) {
            errno = ENAMETOOLONG;
            return -1;
        }
        if (w + len + 1 > buflen) {
            errno = ENAMETOOLONG;
            return -1;
        }
        memcpy(&buf[w], &raw[start], len);
        buf[w + len] = '\0';
        stack[n++] = &buf[w];
        w += len + 1;
    }

    for (r = 0; r < n; r++) {
        comp[r] = stack[r];
    }
    *ncomp_out = n;
    return 0;
}

int sh_path_resolve(const sh_fsctx_t *fs, const char *path, sh_target_t *out)
{
    char raw[SH_POSIX_PATH_MAX];
    char norm[SH_POSIX_PATH_MAX];
    const char *comp[SH_MAX_DEPTH];
    size_t n = 0, i, used;
    const sh_device_t *dev;
    int wrote;

    if (out == NULL) {
        errno = EINVAL;
        return -1;
    }
    out->kind = SH_TARGET_ROOT;
    out->dev = NULL;
    out->sce[0] = '\0';
    out->posix[0] = '\0';

    if (fs == NULL || fs->cwd == NULL || fs->home == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (path == NULL || path[0] == '\0') {
        path = fs->cwd;
    }

    if (path[0] == '~') {
        if (path[1] != '/' && path[1] != '\0') {
            errno = EINVAL; /* ~user: this box has no other users */
            return -1;
        }
        wrote = snprintf(raw, sizeof(raw), "%s%s", fs->home, path + 1);
    } else if (path[0] == '/') {
        wrote = snprintf(raw, sizeof(raw), "%s", path);
    } else {
        wrote = snprintf(raw, sizeof(raw), "%s/%s", fs->cwd, path);
    }
    if (wrote < 0 || (size_t)wrote >= sizeof(raw)) {
        errno = ENAMETOOLONG;
        return -1;
    }

    if (normalize(raw, norm, sizeof(norm), comp, &n) != 0) {
        return -1;
    }
    if (n == 0) {
        out->kind = SH_TARGET_ROOT;
        snprintf(out->posix, sizeof(out->posix), "/");
        return 0;
    }

    dev = sh_device_by_posix(comp[0], strlen(comp[0]));
    if (dev == NULL) {
        /* Never fall back to a default device: a typo must not put a file
         * somewhere the user did not name. */
        errno = ENOENT;
        return -1;
    }
    out->dev = dev;
    out->kind = (n == 1) ? SH_TARGET_DEVICE : SH_TARGET_PATH;

    used = strlen(dev->sce);
    if (used + 1 > sizeof(out->sce)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    memcpy(out->sce, dev->sce, used);
    for (i = 1; i < n; i++) {
        size_t cl = strlen(comp[i]);

        if (used + 1 + cl + 1 > sizeof(out->sce)) {
            errno = ENAMETOOLONG;
            return -1;
        }
        out->sce[used++] = '/';
        memcpy(out->sce + used, comp[i], cl);
        used += cl;
    }
    out->sce[used] = '\0';

    /* The same components again in POSIX spelling: this is what $PWD keeps,
     * so `cd ux0:../..`-style input can never poison the cwd. */
    {
        size_t p = 0;

        for (i = 0; i < n; i++) {
            size_t cl = strlen(comp[i]);

            if (p + 1 + cl + 1 > sizeof(out->posix)) {
                errno = ENAMETOOLONG;
                return -1;
            }
            out->posix[p++] = '/';
            memcpy(out->posix + p, comp[i], cl);
            p += cl;
        }
        out->posix[p] = '\0';
    }
    return 0;
}

int sh_path_sce(const sh_fsctx_t *fs, const char *path, char *out, size_t outlen)
{
    sh_target_t t;

    if (out == NULL || outlen == 0) {
        errno = EINVAL;
        return -1;
    }
    out[0] = '\0';
    if (sh_path_resolve(fs, path, &t) != 0) {
        return -1;
    }
    if (t.kind == SH_TARGET_ROOT) {
        /* "/" is a view of the device table, not a directory anyone can open. */
        errno = ENOTSUP;
        return -1;
    }
    if (strlen(t.sce) + 1 > outlen) {
        errno = ENAMETOOLONG;
        return -1;
    }
    memcpy(out, t.sce, strlen(t.sce) + 1);
    return 0;
}

const char *sh_basename(const char *path)
{
    const char *slash;

    if (path == NULL) {
        return "";
    }
    slash = strrchr(path, '/');
    return (slash == NULL) ? path : slash + 1;
}

int sh_join(char *dst, size_t dstlen, const char *dir, const char *name)
{
    int wrote;

    if (dst == NULL || dir == NULL || name == NULL || dstlen == 0) {
        errno = EINVAL;
        return -1;
    }
    if (name[0] == '\0') {
        wrote = snprintf(dst, dstlen, "%s", dir);
    } else if (strcmp(dir, "/") == 0) {
        wrote = snprintf(dst, dstlen, "/%s", name);
    } else {
        size_t dl = strlen(dir);

        wrote = (dir[dl - 1] == '/')
                    ? snprintf(dst, dstlen, "%s%s", dir, name)
                    : snprintf(dst, dstlen, "%s/%s", dir, name);
    }
    if (wrote < 0 || (size_t)wrote >= dstlen) {
        errno = ENAMETOOLONG;
        return -1;
    }
    return 0;
}

int sh_parent(char *dst, size_t dstlen, const char *path)
{
    const char *slash;
    size_t len;
    int wrote;

    if (dst == NULL || path == NULL || dstlen == 0) {
        errno = EINVAL;
        return -1;
    }
    slash = strrchr(path, '/');
    if (slash == NULL) {
        wrote = snprintf(dst, dstlen, "%s", path);
    } else if (slash == path) {
        wrote = snprintf(dst, dstlen, "/");
    } else {
        len = (size_t)(slash - path);
        if (len >= dstlen) {
            errno = ENAMETOOLONG;
            return -1;
        }
        memcpy(dst, path, len);
        dst[len] = '\0';
        return 0;
    }
    if (wrote < 0 || (size_t)wrote >= dstlen) {
        errno = ENAMETOOLONG;
        return -1;
    }
    return 0;
}

int sh_is_dot(const char *name)
{
    return name != NULL && name[0] == '.' &&
           (name[1] == '\0' || (name[1] == '.' && name[2] == '\0'));
}
