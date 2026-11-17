#ifndef SH_PATH_H
#define SH_PATH_H

/*
 * The one place that knows how a POSIX path becomes a Vita device path.
 *
 * The Vita has no unified tree: every storage area is a separate device named
 * by a "ux0:" style prefix, and sceIo* understands only those prefixes. There
 * is no root, no chdir that two sessions can share safely, and no symlinks.
 * Rather than leak all of that into every builtin and every SFTP handler, the
 * commands speak POSIX and this file is the single translation point.
 *
 * Two rules come from the platform rather than from taste:
 *
 *   - Normalize in POSIX space, translate once, immediately before the sceIo
 *     call. Normalizing in sceIo space is where "ux0:../vs0:" bugs live: the
 *     sceIo path has no root to stop ".." at, so it walks into a sibling
 *     device. Here ".." clamps at the device root, which makes it impossible
 *     for a relative path to leave the device it started on.
 *   - Never guess a device. An unknown first component is ENOENT. Silently
 *     picking a writable device for a bad path turns a typo into a file in
 *     the wrong place.
 */

#include <stddef.h>

/* SceIoDevice is 32 bytes ("ux0:" plus padding) and ScePath is 979, so a
 * device path never needs more than 1024 bytes. */
#define SH_SCE_PATH_MAX 1024
#define SH_POSIX_PATH_MAX 1024

typedef struct {
  const char *sce;    /* "ux0:" */
  const char *posix;  /* "/ux0" */
  /* 0 on the partitions the system mounts read-only. Checked before the
   * syscall so mkdir on /vs0 says "read-only file system" instead of
   * reporting an opaque sceIo error code. */
  int writable;
  /* Removable devices (USB, memory card, host link) only exist while
   * something is in them, so they are probed before being listed. */
  int removable;
} sh_device_t;

/* The shell's read-only pseudo-files under a non-device first component, e.g. /etc/os-release; cat, ls, stat and redirection all ask this table. */
typedef struct {
  const char *dir;   /* POSIX directory it lives in, "/etc" */
  const char *name;  /* "os-release" */
  const char *contents; /* the whole file, already formatted; never NULL */
  size_t size;       /* strlen(contents) */
  int mode;          /* the permission bits stat and ls report */
} sh_pseudo_t;

/* The pseudo-file named by a normalized POSIX path, or NULL; any other name under its directory is ENOENT. */
const sh_pseudo_t *sh_pseudo_lookup(const char *posix);

/* The names `ls /etc` lists, in table order (alphabetical); returns the count. */
size_t sh_pseudo_list(const char *dir, const char *names[], size_t max);

typedef enum {
  SH_TARGET_ROOT = 0,  /* "/", the synthetic listing of the device table */
  SH_TARGET_DEVICE,    /* "/ux0" and "/ux0/" are the same thing */
  SH_TARGET_PATH,      /* "/ux0/vita/save/x.dat" */
  /* A pseudo-file: no sce spelling, so sh_path_sce() fails for it, which keeps it out of SFTP and libc. */
  SH_TARGET_PSEUDO
} sh_target_kind_t;

typedef struct {
  sh_target_kind_t kind;
  const sh_device_t *dev;       /* the device the path sits on; NULL only for
                                 * the synthetic "/" root and for a pseudo-file */
  const sh_pseudo_t *pseudo;    /* the pseudo-file, set only for SH_TARGET_PSEUDO */
  char sce[SH_SCE_PATH_MAX];    /* "ux0:vita/save/x.dat", "" for ROOT and PSEUDO */
  char posix[SH_POSIX_PATH_MAX];/* "/ux0/vita/save/x.dat", "/" for ROOT.
                                 * What $PWD holds: normalized, no trailing
                                 * slash, and the input `cd` was given may
                                 * have had none of that. */
} sh_target_t;

/* Per-call context. The cwd lives here and not in libc because libc's cwd is
 * a single static in lib_a-fs.o shared by the whole process: two sessions
 * calling chdir() would corrupt each other's idea of where they are. */
typedef struct {
  const char *cwd;   /* POSIX, absolute, device-rooted, never NULL */
  const char *home;  /* POSIX, absolute, device-rooted, never NULL */
} sh_fsctx_t;

const sh_device_t *sh_devices(size_t *count);

/* Look up by POSIX name, e.g. ("/ux0", 4). Returns NULL if not a device. */
const sh_device_t *sh_device_by_posix(const char *posix, size_t len);

/* False for a removable device with nothing in it. */
int sh_device_present(const sh_device_t *dev);

/* Resolves path against fs (NULL path or "" means cwd) and fills out.
 * Returns 0, or -1 with errno set: ENOENT for an unknown device or an empty
 * result, ENAMETOOLONG, EINVAL for "~user". A path that names a pseudo-file
 * comes back SH_TARGET_PSEUDO instead of ENOENT. */
int sh_path_resolve(const sh_fsctx_t *fs, const char *path, sh_target_t *out);

/* sh_path_resolve + a copy, for callers that only want the syscall argument.
 * Fails with ENOTSUP when the path resolves to "/": the synthetic root has no
 * sceIo spelling, it is a listing of devices rather than a directory. Fails the
 * same way for a pseudo-file, which has no sceIo spelling either. */
int sh_path_sce(const sh_fsctx_t *fs, const char *path, char *out, size_t outlen);

/* --- POSIX-side string helpers, no syscalls and no device knowledge --- */

/* Last component of a normalized path; "" for a root. */
const char *sh_basename(const char *path);

/* dir + "/" + name, or just name when dir is "/". Returns 0, -1 if too long. */
int sh_join(char *dst, size_t dstlen, const char *dir, const char *name);

/* Everything before the last component. The parent of "/ux0" is "/". */
int sh_parent(char *dst, size_t dstlen, const char *path);

/* True for "." and "..", which no builtin should ever create or remove. */
int sh_is_dot(const char *name);

#endif /* SH_PATH_H */
