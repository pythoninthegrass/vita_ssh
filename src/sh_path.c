/*
 * POSIX path <-> Vita device path. See include/sh_path.h for the rules this
 * file is the only place allowed to implement.
 */
#include "sh_path.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <psp2/io/stat.h>

#include "sshd_sysinfo.h"

/* Synthetic read-only files under a non-device component, served from one table. */

/* A table row before the bytes exist; mode is what stat and ls report. */
typedef struct {
    const char *dir;  /* POSIX directory the name sits in */
    const char *name;
    int mode;
    /* Fills buf with the whole file and returns its length, or -1. */
    int (*render)(char *buf, size_t buflen);
} sh_pseudo_def_t;

/* Renders into buf and returns its length, or -1 to leave the name out of ls. */
static int render_os_release(char *buf, size_t buflen)
{
    static char held[SSHD_OS_RELEASE_MAX];
    static int ready;
    sh_sysinfo_t info;

    if (!ready) {
        sh_sysinfo_get(&info);
        if (sh_sysinfo_os_release(&info, held, sizeof(held)) != 0) {
            held[0] = '\0';
            return -1;
        }
        ready = 1;
    }
    if (strlen(held) + 1 > buflen) {
        errno = ENAMETOOLONG;
        return -1;
    }
    memcpy(buf, held, strlen(held) + 1);
    return (int)strlen(held);
}

static const sh_pseudo_def_t pseudo_defs[] = {
    {"/etc", "os-release", 0444, render_os_release},
};

#define N_PSEUDO (sizeof(pseudo_defs) / sizeof(pseudo_defs[0]))

/* Rendered once and held for the process, so a read needs nothing else alive. */
static sh_pseudo_t pseudo_files[N_PSEUDO];
static int pseudo_built;

static void build_pseudo(void)
{
    char scratch[SSHD_OS_RELEASE_MAX];
    size_t i;

    for (i = 0; i < N_PSEUDO; i++) {
        int len;

        pseudo_files[i].dir = NULL;
        len = pseudo_defs[i].render(scratch, sizeof(scratch));
        if (len < 0) {
            continue;
        }
        pseudo_files[i].contents = strdup(scratch);
        if (pseudo_files[i].contents == NULL) {
            continue;
        }
        pseudo_files[i].dir = pseudo_defs[i].dir;
        pseudo_files[i].name = pseudo_defs[i].name;
        pseudo_files[i].size = (size_t)len;
        pseudo_files[i].mode = pseudo_defs[i].mode;
    }
    pseudo_built = 1;
}

const sh_pseudo_t *sh_pseudo_lookup(const char *posix)
{
    size_t i;

    if (posix == NULL) {
        return NULL;
    }
    if (!pseudo_built) {
        build_pseudo();
    }
    for (i = 0; i < N_PSEUDO; i++) {
        char full[SH_POSIX_PATH_MAX];

        if (pseudo_files[i].dir == NULL) {
            continue;
        }
        if (sh_join(full, sizeof(full), pseudo_files[i].dir,
                    pseudo_files[i].name) != 0) {
            continue;
        }
        if (strcmp(full, posix) == 0) {
            return &pseudo_files[i];
        }
    }
    return NULL;
}

int sh_pseudo_dir_exists(const char *posix)
{
    size_t i;

    if (posix == NULL) {
        return 0;
    }
    if (!pseudo_built) {
        build_pseudo();
    }
    for (i = 0; i < N_PSEUDO; i++) {
        if (pseudo_files[i].dir != NULL && strcmp(pseudo_files[i].dir, posix) == 0) {
            return 1;
        }
    }
    return 0;
}

int sh_pseudo_fd_encode(int slot)
{
    return -(slot + 2);
}

int sh_pseudo_fd_slot(int fd)
{
    return (fd <= -2) ? -fd - 2 : -1;
}

size_t sh_pseudo_list(const char *dir, const char *names[], size_t max)
{
    size_t i, n = 0;

    if (dir == NULL || names == NULL || max == 0) {
        return 0;
    }
    if (!pseudo_built) {
        build_pseudo();
    }
    for (i = 0; i < N_PSEUDO && n < max; i++) {
        if (pseudo_files[i].dir != NULL &&
            strcmp(pseudo_files[i].dir, dir) == 0) {
            names[n++] = pseudo_files[i].name;
        }
    }
    return n;
}

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

/* Splits raw on '/', drops "." and empties, and applies ".." against the stack. */
/* A ".." cancels a named component but never one at or below floor_depth (the device). */
static int normalize(const char *raw, size_t floor_depth, char *buf, size_t buflen,
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
            if (n > floor_depth) {
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
    size_t n = 0, i, used, anchor;
    const sh_device_t *dev;
    int wrote;

    if (out == NULL) {
        errno = EINVAL;
        return -1;
    }
    out->kind = SH_TARGET_ROOT;
    out->dev = NULL;
    out->pseudo = NULL;
    out->sce[0] = '\0';
    out->posix[0] = '\0';

    if (fs == NULL || fs->cwd == NULL || fs->home == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (path == NULL || path[0] == '\0') {
        path = fs->cwd;
    }

    /* Make the path absolute first, prepending exactly one component: the device of the cwd or home. */
    if (path[0] == '~') {
        if (path[1] != '/' && path[1] != '\0') {
            errno = EINVAL; /* ~user: this box has no other users */
            return -1;
        }
        /* "~" alone is home, so it keeps the whole directory; "~/x" is x under it. */
        wrote = snprintf(raw, sizeof(raw), "%s%s", fs->home, path + 1);
    } else if (path[0] == '/') {
        wrote = snprintf(raw, sizeof(raw), "%s", path);
    } else {
        wrote = snprintf(raw, sizeof(raw), "%s/%s", fs->cwd, path);
    }
    anchor = 1; /* the device at index 0, which no ".." may cancel */
    if (wrote < 0 || (size_t)wrote >= sizeof(raw)) {
        errno = ENAMETOOLONG;
        return -1;
    }

    if (normalize(raw, anchor, norm, sizeof(norm), comp, &n) != 0) {
        return -1;
    }
    if (n == 0) {
        out->kind = SH_TARGET_ROOT;
        snprintf(out->posix, sizeof(out->posix), "/");
        return 0;
    }

    /* Only an unknown first component can name a pseudo-file, so /ux0/etc is ux0. */
    {
        char full[SH_POSIX_PATH_MAX];
        size_t p = 0;

        for (i = 0; i < n; i++) {
            size_t cl = strlen(comp[i]);

            if (p + 1 + cl + 1 > sizeof(full)) {
                errno = ENAMETOOLONG;
                return -1;
            }
            full[p++] = '/';
            memcpy(full + p, comp[i], cl);
            p += cl;
        }
        full[p] = '\0';
        out->pseudo = sh_pseudo_lookup(full);
        if (out->pseudo != NULL) {
            out->kind = SH_TARGET_PSEUDO;
            snprintf(out->posix, sizeof(out->posix), "%s", full);
            return 0;
        }
        if (sh_pseudo_dir_exists(full)) {
            out->kind = SH_TARGET_PSEUDO_DIR;
            snprintf(out->posix, sizeof(out->posix), "%s", full);
            return 0;
        }
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
    if (t.kind == SH_TARGET_ROOT || t.kind == SH_TARGET_PSEUDO ||
        t.kind == SH_TARGET_PSEUDO_DIR) {
        /* None has a sceIo spelling; failing beats passing "" on to libc. */
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
