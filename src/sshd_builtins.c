/*
 * The builtin shell.
 *
 * Shape taken from a-Shell: a command is a C function in a static table, run
 * in-process, because there is nothing to fork or exec here. Unlike a-Shell
 * there is no sandbox to model -- every Vita device is reachable, presented as
 * a top-level directory -- and unlike a-Shell the output has to find its way to
 * a socket rather than to a terminal widget, which is why nothing in this file
 * may call printf: that is the debug screen, not the user.
 *
 * Everything here is plain POSIX (open/read/stat/opendir/unlink/rename), which
 * works because VitaSDK's newlib maps those onto sceIo* in lib_a-syscalls.o.
 * The only Vita-specific call is the sceIoDevctl one behind df, and every path
 * string handed to libc comes out of sh_path.c.
 */
#include "sshd.h"
#include "sh_alias.h"
#include "sh_chain.h"
#include "sshd_curl.h"
#include "sh_grep.h"
#include "sh_headtail.h"
#include "sh_textops.h"
#include "sh_wget_args.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/param.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <psp2/io/devctl.h>

#include "sshd_sysinfo.h"

#ifndef NAME_MAX
#define NAME_MAX 255
#endif

#define MAX_ARGS 64
#define MAX_LIST 512

/* Recursion bound for the tree walks. A device with a 25-deep tree is not a
 * thing anyone is going to rm -r from a terminal, and the bound keeps a
 * corrupt directory cycle from running the stack off. */
#define MAX_DEPTH 24

/* 0x3001 is the devctl code that fills a SceIoDevInfo, per the example in
 * psp2/io/devctl.h. VitaShell uses the same code for its free-space checks. */
#define DEVCTL_GET_DEV_INFO 0x3001

/* One pair of path scratch buffers, heap-allocated by the command that starts
 * a walk and shared down the recursion. Sharing is safe because every walk
 * rebuilds the name it needs at the top of each iteration, so nothing is read
 * after a recursive call clobbers it. */
typedef struct {
    char a[SH_SCE_PATH_MAX];
    char b[SH_SCE_PATH_MAX];
    char pa[SH_POSIX_PATH_MAX];
    char pb[SH_POSIX_PATH_MAX];
} sh_walk_t;

/* ------------------------------------------------------------------ */
/* helpers                                                              */
/* ------------------------------------------------------------------ */

static void fs_of(const sh_session_t *s, sh_fsctx_t *fs)
{
    sh_session_get_fsctx(s, fs);
}

/* Resolves a user-supplied path and reports a failure in the command's own
 * voice, which is the form of error message people actually read. */
static int resolve_arg(sh_session_t *s, const char *cmd, const char *arg,
                       sh_target_t *t)
{
    sh_fsctx_t fs;

    fs_of(s, &fs);
    if (sh_path_resolve(&fs, arg, t) != 0) {
        sh_eprintf(s, "%s: %s: %s\n", cmd, arg ? arg : ".", strerror(errno));
        return -1;
    }
    return 0;
}

/* Refuses a mutation before it reaches the syscall, so a write to /os0 says
 * "read-only file system" instead of reporting a sceIo code nobody can look up. */
static int check_writable(sh_session_t *s, const char *cmd, const char *arg,
                          const sh_target_t *t)
{
    if (t->kind == SH_TARGET_PSEUDO || t->kind == SH_TARGET_PSEUDO_DIR) {
        /* Synthetic files report the firmware; a real file beside them would persist. */
        sh_eprintf(s, "%s: %s: Permission denied\n", cmd, arg ? arg : ".");
        return -1;
    }
    if (t->kind == SH_TARGET_ROOT || t->dev == NULL || !t->dev->writable) {
        sh_eprintf(s, "%s: %s: Read-only file system\n", cmd, arg ? arg : ".");
        return -1;
    }
    return 0;
}

static int stat_target(const sh_target_t *t, struct stat *st)
{
    if (t->kind == SH_TARGET_ROOT || t->kind == SH_TARGET_PSEUDO_DIR) {
        memset(st, 0, sizeof(*st));
        st->st_mode = (mode_t)(S_IFDIR | 0555);
        return 0;
    }
    if (t->kind == SH_TARGET_PSEUDO) {
        /* A synthetic regular file; no mtime, since the clock says nothing about it. */
        memset(st, 0, sizeof(*st));
        st->st_mode = (mode_t)(S_IFREG | t->pseudo->mode);
        st->st_size = (off_t)t->pseudo->size;
        return 0;
    }
    return stat(t->sce, st);
}

/* Pseudo-file fds: slots over a buffer, since a libc fd cannot name one. */
#define PSEUDO_SLOTS 8

typedef struct {
    const sh_pseudo_t *file;
    size_t off;
} pseudo_slot_t;

static pseudo_slot_t pseudo_slots[PSEUDO_SLOTS];

static int pseudo_open(const sh_pseudo_t *file)
{
    int i;

    for (i = 0; i < PSEUDO_SLOTS; i++) {
        if (pseudo_slots[i].file == NULL) {
            pseudo_slots[i].file = file;
            pseudo_slots[i].off = 0;
            return sh_pseudo_fd_encode(i);
        }
    }
    errno = EMFILE;
    return -1;
}

static int pseudo_close(int fd)
{
    int i = sh_pseudo_fd_slot(fd);

    if (i < 0 || i >= PSEUDO_SLOTS) {
        errno = EBADF;
        return -1;
    }
    pseudo_slots[i].file = NULL;
    return 0;
}

static ssize_t pseudo_read(int fd, void *buf, size_t len)
{
    pseudo_slot_t *slot;
    size_t left, n;
    int i = sh_pseudo_fd_slot(fd);

    if (i < 0 || i >= PSEUDO_SLOTS) {
        errno = EBADF;
        return -1;
    }
    slot = &pseudo_slots[i];
    if (slot->file == NULL) {
        errno = EBADF;
        return -1;
    }
    left = slot->file->size - slot->off;
    n = (len < left) ? len : left;
    memcpy(buf, slot->file->contents + slot->off, n);
    slot->off += n;
    return (ssize_t)n;
}

/* open/read/close routed by target kind; a negative fd is a pseudo-file slot. */
static int sh_target_open(const sh_target_t *t)
{
    return (t->kind == SH_TARGET_PSEUDO) ? pseudo_open(t->pseudo)
                                         : open(t->sce, O_RDONLY);
}

static ssize_t sh_target_read(int fd, void *buf, size_t len)
{
    return (sh_pseudo_fd_slot(fd) >= 0) ? pseudo_read(fd, buf, len) : read(fd, buf, len);
}

static int sh_target_close(int fd)
{
    return (sh_pseudo_fd_slot(fd) >= 0) ? pseudo_close(fd) : close(fd);
}

static void human_size(char *dst, size_t dstlen, long long bytes)
{
    static const char *units[] = {"B", "K", "M", "G", "T"};
    double v = (double)bytes;
    size_t u = 0;

    while (v >= 1024.0 && u + 1 < sizeof(units) / sizeof(units[0])) {
        v /= 1024.0;
        u++;
    }
    if (u == 0) {
        snprintf(dst, dstlen, "%lld%s", bytes, units[u]);
    } else {
        snprintf(dst, dstlen, "%.1f%s", v, units[u]);
    }
}

static const char *mode_string(mode_t mode, char buf[11])
{
    int i;

    buf[0] = S_ISDIR(mode) ? 'd' : (S_ISLNK(mode) ? 'l' : '-');
    for (i = 0; i < 9; i++) {
        buf[i + 1] = (mode & (mode_t)(0400 >> i)) ? (char)("rwx"[i % 3]) : '-';
    }
    buf[10] = '\0';
    return buf;
}

/*
 * The Vita has no per-file permission bits to show: the memory card is FAT and
 * stat() fills st_mode with the file type and nothing else, so mode_string()
 * above would render every entry as "---------", which reads as broken rather
 * than as "this platform has no bits here". What the platform does enforce is
 * the device mount -- a read-only device (os0, vs0, gro0) refuses writes no
 * matter what the bits say -- so synthesize the mode from the two things that
 * are true: the type, from stat, and the write bit, from the device table.
 * `dr-xr-xr-x` on /vs0 and `drwxr-xr-x` on /ux0 describe exactly what a client
 * of this shell can and cannot do, which is more than nine dashes said.
 */
static mode_t synth_mode(const struct stat *st, const sh_device_t *dev)
{
    mode_t mode = S_ISDIR(st->st_mode) ? (mode_t)0755 : (mode_t)0644;

    if (dev != NULL && !dev->writable) {
        mode &= (mode_t)~0222;
    }
    return mode;
}

static void format_time(char *dst, size_t dstlen, time_t when, const char *fmt)
{
    struct tm *tm;

    snprintf(dst, dstlen, "-");
    if (when == 0) {
        return;
    }
    tm = localtime(&when);
    if (tm != NULL && strftime(dst, dstlen, fmt, tm) > 0) {
        return;
    }
    snprintf(dst, dstlen, "-");
}

/* ------------------------------------------------------------------ */
/* wildcard matching and expansion                                      */
/* ------------------------------------------------------------------ */

/* * and ? and [a-z][0-9-], which is what a person types at a prompt. A name
 * starting with '.' only matches a pattern starting with '.', so `*` hides
 * dotfiles the way sh does. */
static int match_pattern(const char *pat, const char *name)
{
    while (*pat != '\0') {
        switch (*pat) {
        case '?':
            if (*name == '\0') {
                return 0;
            }
            pat++;
            name++;
            break;
        case '*':
            while (*pat == '*') {
                pat++;
            }
            if (*pat == '\0') {
                return 1;
            }
            for (; *name != '\0'; name++) {
                if (match_pattern(pat, name)) {
                    return 1;
                }
            }
            return 0;
        case '[': {
            const char *p = pat + 1;
            int negated = 0, matched = 0;

            if (*p == '!') {
                negated = 1;
                p++;
            }
            while (*p != '\0' && *p != ']') {
                char lo = *p++, hi = lo;

                if (*p == '-' && p[1] != '\0' && p[1] != ']') {
                    p++;
                    hi = *p++;
                }
                if (*name >= lo && *name <= hi) {
                    matched = 1;
                }
            }
            if (*p != ']') { /* unterminated class: treat '[' as literal */
                if (*name != '[') {
                    return 0;
                }
                pat++;
                name++;
                break;
            }
            if (matched == negated) {
                return 0;
            }
            if (*name == '\0') {
                return 0;
            }
            pat = p + 1;
            name++;
            break;
        }
        default:
            if (*name != *pat) {
                return 0;
            }
            pat++;
            name++;
            break;
        }
    }
    return *name == '\0';
}

static int has_glob_chars(const char *s)
{
    return strpbrk(s, "*?[") != NULL;
}

/*
 * Expands one word against the filesystem. The directory part goes through the
 * path layer and the matched components are appended in POSIX space, so a
 * pattern can never reach a device other than the one it started on. With no
 * match the word is kept verbatim, which is what sh does and what makes
 * `echo *.typo` tell you something instead of failing.
 */
static int expand_glob(const sh_fsctx_t *fs, const char *word, char **out,
                       int max)
{
    char dirpart[SH_POSIX_PATH_MAX];
    char sce_dir[SH_SCE_PATH_MAX];
    const char *base, *pattern, *slash;
    DIR *dir;
    struct dirent *ent;
    int found = 0;

    slash = strrchr(word, '/');
    if (slash == NULL) {
        pattern = word;
        base = fs->cwd;
        if (sh_path_sce(fs, NULL, sce_dir, sizeof(sce_dir)) != 0) {
            return 0;
        }
    } else {
        sh_target_t parent;
        size_t dirlen = (size_t)(slash - word);

        pattern = slash + 1;
        if (pattern[0] == '\0') {
            return 0; /* trailing slash: no component left to match */
        }
        if (dirlen >= sizeof(dirpart)) {
            return 0;
        }
        memcpy(dirpart, word, dirlen);
        dirpart[dirlen] = '\0';
        if (sh_path_resolve(fs, dirpart, &parent) != 0) {
            return 0;
        }
        /* No directory stream behind either name, so `*` expands to nothing. */
        if (parent.kind == SH_TARGET_ROOT || parent.kind == SH_TARGET_PSEUDO ||
            parent.kind == SH_TARGET_PSEUDO_DIR) {
            return 0;
        }
        base = dirpart;
        if (strlen(parent.sce) + 1 > sizeof(sce_dir)) {
            return 0;
        }
        memcpy(sce_dir, parent.sce, strlen(parent.sce) + 1);
    }

    dir = opendir(sce_dir);
    if (dir == NULL) {
        return 0;
    }
    while ((ent = readdir(dir)) != NULL && found < max) {
        char *candidate = malloc(SH_POSIX_PATH_MAX);

        if (candidate == NULL) {
            break;
        }
        if (ent->d_name[0] == '.' && pattern[0] != '.') {
            free(candidate);
            continue;
        }
        if (!match_pattern(pattern, ent->d_name)) {
            free(candidate);
            continue;
        }
        /* The result is a path the user sees and may edit, so it is built in
         * POSIX space, not as a syscall argument. */
        if (sh_join(candidate, SH_POSIX_PATH_MAX, base, ent->d_name) != 0) {
            free(candidate);
            continue;
        }
        out[found++] = candidate;
    }
    closedir(dir);
    return found;
}

/* ------------------------------------------------------------------ */
/* line parsing: quotes first, then globs                               */
/* ------------------------------------------------------------------ */

static int split_line(const char *line, char **argv, int *quoted, int max)
{
    int argc = 0;
    const char *p = line;

    while (*p != '\0' && argc < max) {
        char *word, *w;
        int saw_quote = 0;

        while (*p == ' ' || *p == '\t') {
            p++;
        }
        if (*p == '\0') {
            break;
        }
        word = malloc(strlen(p) + 1);
        if (word == NULL) {
            break;
        }
        w = word;
        while (*p != '\0' && *p != ' ' && *p != '\t') {
            if (*p == '\'' || *p == '"') {
                char quote = *p++;

                saw_quote = 1;
                while (*p != '\0' && *p != quote) {
                    *w++ = *p++;
                }
                if (*p == quote) {
                    p++;
                }
                continue;
            }
            if (*p == '\\' && p[1] != '\0') {
                p++;
                saw_quote = 1;
            }
            *w++ = *p++;
        }
        *w = '\0';
        argv[argc] = word;
        if (quoted != NULL) {
            quoted[argc] = saw_quote;
        }
        argc++;
    }
    return argc;
}

static void free_args(char **argv, int argc)
{
    int i;

    for (i = 0; i < argc; i++) {
        free(argv[i]);
        argv[i] = NULL;
    }
}

/* Pulls the non-option arguments out of argv, skipping -flags. Returns their
 * count and, unless rejected is NULL, notes that a bad flag was seen. */
static int collect_targets(char **argv, int argc, const char *flags,
                           const char **targets, int max, int *bad_flag)
{
    int i, n = 0;

    for (i = 1; i < argc; i++) {
        if (argv[i][0] == '-' && argv[i][1] != '\0' && flags != NULL) {
            const char *f;

            for (f = argv[i] + 1; *f != '\0'; f++) {
                if (strchr(flags, *f) == NULL) {
                    if (bad_flag != NULL) {
                        *bad_flag = *f;
                    }
                    return n;
                }
            }
            continue;
        }
        if (n < max) {
            targets[n++] = argv[i];
        }
    }
    return n;
}

/* ------------------------------------------------------------------ */
/* pwd, cd                                                              */
/* ------------------------------------------------------------------ */

static int cmd_pwd(sh_session_t *s, int argc, char **argv)
{
    sh_fsctx_t fs;

    (void)argc;
    (void)argv;
    fs_of(s, &fs);
    sh_printf(s, "%s\n", fs.cwd);
    return 0;
}

static int cmd_cd(sh_session_t *s, int argc, char **argv)
{
    sh_target_t t;

    if (argc > 2) {
        sh_eprintf(s, "cd: too many arguments\n");
        return 1;
    }
    /* Bare `cd` means home, and the resolver maps "~" there; passing it the
     * NULL that `ls` hands over for "no argument" would make `cd` resolve to
     * the cwd and quietly do nothing. */
    if (resolve_arg(s, "cd", (argc > 1) ? argv[1] : "~", &t) != 0) {
        return 1;
    }
    if (t.kind == SH_TARGET_ROOT) {
        sh_session_set_cwd(s, "/");
        return 0;
    }
    if (t.kind == SH_TARGET_PSEUDO_DIR) {
        sh_eprintf(s, "cd: %s: %s\n", argv[1], strerror(ENOTSUP));
        return 1;
    }
    {
        struct stat st;

        if (stat_target(&t, &st) != 0) {
            sh_eprintf(s, "cd: %s: %s\n", argv[1], strerror(errno));
            return 1;
        }
        if (!S_ISDIR(st.st_mode)) {
            sh_eprintf(s, "cd: %s: Not a directory\n", argv[1]);
            return 1;
        }
    }
    /* The normalized target, never the typed text: $PWD must come out the same
     * after `cd x`, `cd ./x/` and `cd //ux0//data`. */
    sh_session_set_cwd(s, t.posix);
    return 0;
}

/* ------------------------------------------------------------------ */
/* ls                                                                   */
/* ------------------------------------------------------------------ */

/* One ls -l line; sce_path is NULL for a pseudo-file, which carries its own mode. */
static void print_long(sh_session_t *s, const char *sce_path,
                       const char *display, const sh_device_t *dev,
                       const sh_pseudo_t *pseudo)
{
    struct stat st;
    char perms[11], when[32], size[16];
    mode_t mode;

    if (pseudo != NULL) {
        memset(&st, 0, sizeof(st));
        st.st_mode = (mode_t)(S_IFREG | pseudo->mode);
        st.st_size = (off_t)pseudo->size;
    } else if (stat(sce_path, &st) != 0) {
        sh_printf(s, "?  %s\n", display);
        return;
    }
    mode = (pseudo != NULL) ? st.st_mode : synth_mode(&st, dev);
    format_time(when, sizeof(when), st.st_mtime, "%b %e %H:%M");
    human_size(size, sizeof(size), (long long)st.st_size);
    sh_printf(s, "%s %6s %s  %s%s\n",
              mode_string(mode, perms), size, when,
              display, S_ISDIR(st.st_mode) ? "/" : "");
}

static int compare_names(const void *a, const void *b)
{
    return strcmp((const char *)a, (const char *)b);
}

/* Lists one device or directory. Returns 0, or 1 after reporting the error. */
static int list_one(sh_session_t *s, const char *cmd, const sh_target_t *t,
                    int long_format, int show_hidden)
{
    char (*names)[NAME_MAX + 1];
    int n = 0, i, col = 0, width = 0, cols = sh_session_cols(s), rc = 0;
    DIR *dir;
    struct dirent *ent;

    names = malloc((size_t)MAX_LIST * (NAME_MAX + 1));
    if (names == NULL) {
        sh_eprintf(s, "%s: out of memory\n", cmd);
        return 1;
    }
    dir = opendir(t->sce);
    if (dir == NULL) {
        free(names);
        sh_eprintf(s, "%s: %s: %s\n", cmd, t->posix, strerror(errno));
        return 1;
    }
    while ((ent = readdir(dir)) != NULL && n < MAX_LIST) {
        if (!show_hidden && ent->d_name[0] == '.') {
            continue;
        }
        snprintf(names[n], NAME_MAX + 1, "%s", ent->d_name);
        if ((int)strlen(ent->d_name) > width) {
            width = (int)strlen(ent->d_name);
        }
        n++;
    }
    closedir(dir);
    qsort(names, (size_t)n, NAME_MAX + 1, compare_names);

    if (long_format || n == 0) {
        for (i = 0; i < n; i++) {
            char sce[SH_SCE_PATH_MAX];

            if (sh_join(sce, sizeof(sce), t->sce, names[i]) != 0) {
                rc = 1;
                continue;
            }
            print_long(s, sce, names[i], t->dev, NULL);
        }
        free(names);
        return rc;
    }
    {
        int per_row = (cols > width + 2) ? (cols / (width + 2)) : 1;

        for (i = 0; i < n; i++) {
            sh_printf(s, "%-*s", width + 2, names[i]);
            if (++col >= per_row) {
                sh_printf(s, "\n");
                col = 0;
            }
        }
        if (col != 0) {
            sh_printf(s, "\n");
        }
    }
    free(names);
    return 0;
}

/* The synthetic root: the device table, filtered to what is mounted now. */
static int list_root(sh_session_t *s)
{
    const sh_device_t *devices;
    size_t count, i;

    devices = sh_devices(&count);
    for (i = 0; i < count; i++) {
        if (!sh_device_present(&devices[i])) {
            continue;
        }
        sh_printf(s, "%-8s  %s\n", devices[i].posix,
                  devices[i].writable ? "" : "read-only");
    }
    return 0;
}

/* Lists the pseudo-file table for `ls /etc`, in table order. */
static int list_pseudo_dir(sh_session_t *s, const char *dir, int long_format)
{
    const char *names[MAX_LIST];
    int col = 0, width = 0, cols = sh_session_cols(s), rc = 0;
    size_t got, count;
    int per_row;

    count = sh_pseudo_list(dir, names, (size_t)MAX_LIST);
    for (got = 0; got < count; got++) {
        if ((int)strlen(names[got]) > width) {
            width = (int)strlen(names[got]);
        }
    }
    if (long_format) {
        for (got = 0; got < count; got++) {
            char full[SH_POSIX_PATH_MAX];
            const sh_pseudo_t *file;

            if (sh_join(full, sizeof(full), dir, names[got]) != 0) {
                rc = 1;
                continue;
            }
            file = sh_pseudo_lookup(full);
            if (file == NULL) {
                rc = 1;
                continue;
            }
            print_long(s, NULL, names[got], NULL, file);
        }
        return rc;
    }
    per_row = (cols > width + 2) ? (cols / (width + 2)) : 1;
    for (got = 0; got < count; got++) {
        sh_printf(s, "%-*s", width + 2, names[got]);
        if (++col >= per_row) {
            sh_printf(s, "\n");
            col = 0;
        }
    }
    if (col != 0) {
        sh_printf(s, "\n");
    }
    return rc;
}

/* ls of one argument: device table for "/", pseudo-file table, or a dir stream. */
static int list_any(sh_session_t *s, const char *cmd, const sh_target_t *t,
                    int long_format, int show_hidden)
{
    if (t->kind == SH_TARGET_ROOT) {
        return list_root(s);
    }
    if (t->kind == SH_TARGET_PSEUDO_DIR) {
        return list_pseudo_dir(s, t->posix, long_format);
    }
    if (t->dev == NULL) {
        sh_eprintf(s, "%s: %s: %s\n", cmd, t->posix, strerror(ENOENT));
        return 1;
    }
    return list_one(s, cmd, t, long_format, show_hidden);
}

static int cmd_ls(sh_session_t *s, int argc, char **argv)
{
    int long_format = 0, show_hidden = 0, i, status = 0, bad = 0, paths;
    const char *targets[MAX_ARGS];

    for (i = 1; i < argc; i++) {
        const char *f;

        if (argv[i][0] != '-' || argv[i][1] == '\0') {
            break;
        }
        for (f = argv[i] + 1; *f != '\0'; f++) {
            if (*f == 'l') {
                long_format = 1;
            } else if (*f == 'a') {
                show_hidden = 1;
            } else if (*f == 'h') {
                /* accepted and meaningless: sizes are already human */
            } else {
                bad = *f;
            }
        }
    }
    if (bad != 0) {
        sh_eprintf(s, "ls: illegal option -- %c\n", bad);
        return 1;
    }
    paths = 0;
    for (; i < argc; i++) {
        if (paths < MAX_ARGS) {
            targets[paths++] = argv[i];
        }
    }

    if (paths == 0) {
        sh_target_t t;

        if (resolve_arg(s, "ls", NULL, &t) != 0) {
            return 1;
        }
        return list_any(s, "ls", &t, long_format, show_hidden);
    }
    for (i = 0; i < paths; i++) {
        sh_target_t t;
        struct stat st;

        if (resolve_arg(s, "ls", targets[i], &t) != 0) {
            status = 1;
            continue;
        }
        /* A pseudo-file prints as one line, never as its directory's listing. */
        if (t.kind == SH_TARGET_PSEUDO) {
            if (paths > 1) {
                sh_printf(s, "%s:\n", targets[i]);
            }
            if (long_format) {
                print_long(s, NULL, sh_basename(t.posix), NULL, t.pseudo);
            } else {
                sh_printf(s, "%s\n", sh_basename(t.posix));
            }
            continue;
        }
        if (t.kind == SH_TARGET_ROOT || t.kind == SH_TARGET_PSEUDO_DIR) {
            if (paths > 1) {
                sh_printf(s, "%s:\n", targets[i]);
            }
            if (list_any(s, "ls", &t, long_format, show_hidden) != 0) {
                status = 1;
            }
            continue;
        }
        if (stat_target(&t, &st) != 0) {
            sh_eprintf(s, "ls: %s: %s\n", targets[i], strerror(errno));
            status = 1;
            continue;
        }
        if (paths > 1) {
            sh_printf(s, "%s:\n", targets[i]);
        }
        if (S_ISDIR(st.st_mode)) {
            if (list_any(s, "ls", &t, long_format, show_hidden) != 0) {
                status = 1;
            }
            continue;
        }
        /* A file argument prints as one line, like ls(1): the name, or the
         * long form when -l was asked for. */
        if (long_format) {
            print_long(s, t.sce, sh_basename(t.posix), t.dev, NULL);
        } else {
            sh_printf(s, "%s\n", sh_basename(t.posix));
        }
    }
    return status;
}

/* ------------------------------------------------------------------ */
/* cat, echo                                                            */
/* ------------------------------------------------------------------ */

static int cmd_cat(sh_session_t *s, int argc, char **argv)
{
    char *buf;
    int i, status = 0;

    buf = malloc(SSHD_COPY_BUFSIZE);
    if (buf == NULL) {
        sh_eprintf(s, "cat: out of memory\n");
        return 1;
    }
    if (argc < 2) {
        size_t got;

        while ((got = sh_session_stdin_read(s, buf, SSHD_COPY_BUFSIZE)) > 0 &&
               !sh_session_interrupted(s)) {
            sh_write(s, buf, got);
        }
        free(buf);
        return 0;
    }
    for (i = 1; i < argc; i++) {
        sh_target_t t;
        ssize_t got;
        int fd;

        if (resolve_arg(s, "cat", argv[i], &t) != 0) {
            status = 1;
            continue;
        }
        if (t.kind == SH_TARGET_ROOT || t.kind == SH_TARGET_PSEUDO_DIR) {
            sh_eprintf(s, "cat: %s: Is a directory\n", argv[i]);
            status = 1;
            continue;
        }
        fd = sh_target_open(&t);
        if (fd == -1) {
            sh_eprintf(s, "cat: %s: %s\n", argv[i], strerror(errno));
            status = 1;
            continue;
        }
        while ((got = sh_target_read(fd, buf, SSHD_COPY_BUFSIZE)) > 0 &&
               !sh_session_interrupted(s)) {
            sh_write(s, buf, (size_t)got);
        }
        sh_target_close(fd);
        if (got < 0) {
            sh_eprintf(s, "cat: %s: %s\n", argv[i], strerror(errno));
            status = 1;
        }
    }
    free(buf);
    return status;
}

/* ------------------------------------------------------------------ */
/* grep                                                                 */
/* ------------------------------------------------------------------ */

/* grep reads fd or stdin through one byte callback so SH_GREP_LINE_MAX is enforced in one place. */
typedef struct {
    sh_session_t *s;
    int fd;            /* >= 0 for a file, -1 for stdin */
    int file_eof;      /* the file has been drained */
    char *rbuf;        /* read-ahead for a file, NULL to read straight through */
    size_t rpos, rlen;
} grep_src_t;

#define GREP_RBUF 4096

/* Pulls up to n bytes from a file or stdin into p. Returns the count, or 0 at end. */
static size_t grep_fill(void *vsrc, char *p, size_t n)
{
    grep_src_t *src = vsrc;

    if (src->fd >= 0) {
        ssize_t got;

        if (src->file_eof) {
            return 0;
        }
        if (src->rbuf == NULL) {
            got = sh_target_read(src->fd, p, n);
            if (got <= 0) {
                src->file_eof = 1;
                return 0;
            }
            return (size_t)got;
        }
        if (src->rpos == src->rlen) {
            got = sh_target_read(src->fd, src->rbuf, GREP_RBUF);
            if (got <= 0) {
                src->file_eof = 1;
                return 0;
            }
            src->rlen = (size_t)got;
            src->rpos = 0;
        }
        if (n > src->rlen - src->rpos) {
            n = src->rlen - src->rpos;
        }
        memcpy(p, src->rbuf + src->rpos, n);
        src->rpos += n;
        return n;
    }
    return sh_session_stdin_read(src->s, p, n);
}

/* Lines are batched into one buffer so a 20000-line answer is a few writes, not 60000. */
typedef struct {
    sh_session_t *s;
    int with_count; /* uniq -c: prefix the seven-wide run count */
    size_t used;
    char buf[2048];
} to_sink_t;

static void to_flush(to_sink_t *sink)
{
    if (sink->used > 0) {
        sh_write(sink->s, sink->buf, sink->used);
        sink->used = 0;
    }
}

static void to_put(to_sink_t *sink, const char *data, size_t len)
{
    if (len > sizeof(sink->buf) - sink->used) {
        to_flush(sink);
    }
    if (len > sizeof(sink->buf)) {
        sh_write(sink->s, data, len);
        return;
    }
    memcpy(sink->buf + sink->used, data, len);
    sink->used += len;
}

/* One match line into the batch: [file:][lineno:]line, newline-terminated. */
static void grep_put(to_sink_t *gs, const char *file, int numbering, long lineno,
                     const char *line)
{
    if (file != NULL) {
        to_put(gs, file, strlen(file));
        to_put(gs, ":", 1);
    }
    if (numbering) {
        char num[24];
        int n = snprintf(num, sizeof(num), "%ld:", lineno);

        to_put(gs, num, (size_t)n);
    }
    to_put(gs, line, strlen(line));
    to_put(gs, "\n", 1);
}

static int cmd_grep(sh_session_t *s, int argc, char **argv)
{
    int icase = 0, invert = 0, numbering = 0, counting = 0, only_files = 0;
    int quiet = 0, i, bad = 0, status = 2, files = 0, matched_any = 0, had_error = 0;
    const char *pattern = NULL;
    const char *targets[MAX_ARGS];
    char *line;
    sh_grep_t *re;
    char why[96];
    to_sink_t gs;

    gs.s = s;
    gs.with_count = 0;
    gs.used = 0;

    for (i = 1; i < argc; i++) {
        const char *f;

        if (argv[i][0] != '-' || argv[i][1] == '\0') {
            break;
        }
        for (f = argv[i] + 1; *f != '\0'; f++) {
            switch (*f) {
            case 'i': icase = 1; break;
            case 'v': invert = 1; break;
            case 'n': numbering = 1; break;
            case 'c': counting = 1; break;
            case 'l': only_files = 1; break;
            case 'q': quiet = 1; break;
            default: bad = *f; break;
            }
        }
    }
    if (bad != 0) {
        sh_eprintf(s, "grep: illegal option -- %c\n", bad);
        return 2;
    }
    if (i >= argc) {
        sh_eprintf(s, "usage: grep [-ivnclq] PATTERN [FILE...]\n");
        sh_eprintf(s, "grep: %s\n", "missing pattern operand");
        return 2;
    }
    pattern = argv[i++];
    for (; i < argc && files < MAX_ARGS; i++) {
        targets[files++] = argv[i];
    }

    re = sh_grep_compile(pattern, icase ? SH_GREP_ICASE : 0);
    if (re == NULL) {
        sh_grep_compile_error(why, sizeof(why));
        sh_eprintf(s, "grep: %s: %s\n", pattern, why[0] != '\0' ? why : "invalid pattern");
        return 2;
    }
    line = malloc(SH_GREP_LINE_MAX);
    if (line == NULL) {
        sh_grep_free(re);
        sh_eprintf(s, "grep: out of memory\n");
        return 2;
    }

    if (files == 0) {
        grep_src_t src;
        long lineno = 0;
        long count = 0;
        int n;
        int err = 0;
        int eof = 0;

        src.s = s;
        src.fd = -1;
        src.file_eof = 0;
        src.rbuf = NULL;
        while (!sh_session_interrupted(s)) {
            int hit;

            n = sh_grep_read_line(grep_fill, &src, line, SH_GREP_LINE_MAX, &eof);
            if (eof) {
                break;
            }
            lineno++;
            if (n < 0) {
                to_flush(&gs);
                sh_eprintf(s, "grep: (standard input): line %ld: too long (limit %d bytes)\n",
                           lineno, SH_GREP_LINE_MAX);
                err = 1;
                continue;
            }
            hit = sh_grep_match(re, line, invert);
            if (hit < 0) {
                to_flush(&gs);
                sh_eprintf(s, "grep: %s: pattern too expensive to match\n", pattern);
                err = 1;
                break;
            }
            if (hit) {
                matched_any = 1;
                count++;
                if (quiet) {
                    /* -q says nothing and only sets the status: stop at the first hit. */
                    break;
                }
                if (!counting && !only_files) {
                    grep_put(&gs, NULL, numbering, lineno, line);
                }
            }
        }
        to_flush(&gs);
        if (counting && !quiet) {
            sh_printf(s, "%ld\n", count);
        }
        free(line);
        sh_grep_free(re);
        if (quiet) {
            return matched_any ? 0 : 1;
        }
        if (err) {
            return 2;
        }
        return matched_any ? 0 : 1;
    }

    /* Several files: -l names matches, a match gets a file: prefix, an unreadable file is exit 2 */
    status = 1;
    had_error = 0;
    for (i = 0; i < files; i++) {
        sh_target_t t;
        struct stat st;
        grep_src_t src;
        long lineno = 0;
        long count = 0;
        int fd, n, file_hit = 0;
        int eof = 0;

        if (resolve_arg(s, "grep", targets[i], &t) != 0) {
            had_error = 1;
            continue;
        }
        if (t.kind == SH_TARGET_ROOT || t.kind == SH_TARGET_PSEUDO_DIR) {
            sh_eprintf(s, "grep: %s: Is a directory\n", targets[i]);
            had_error = 1;
            continue;
        }
        if (stat_target(&t, &st) == 0 && S_ISDIR(st.st_mode)) {
            sh_eprintf(s, "grep: %s: Is a directory\n", targets[i]);
            had_error = 1;
            continue;
        }
        fd = sh_target_open(&t);
        if (fd == -1) {
            sh_eprintf(s, "grep: %s: %s\n", targets[i], strerror(errno));
            had_error = 1;
            continue;
        }
        src.s = s;
        src.fd = fd;
        src.file_eof = 0;
        src.rbuf = malloc(GREP_RBUF);
        src.rpos = src.rlen = 0;
        while (!sh_session_interrupted(s)) {
            int hit;

            n = sh_grep_read_line(grep_fill, &src, line, SH_GREP_LINE_MAX, &eof);
            if (eof) {
                break;
            }
            lineno++;
            if (n < 0) {
                to_flush(&gs);
                sh_eprintf(s, "grep: %s: line %ld: too long (limit %d bytes)\n",
                           targets[i], lineno, SH_GREP_LINE_MAX);
                had_error = 1;
                continue;
            }
            hit = sh_grep_match(re, line, invert);
            if (hit < 0) {
                to_flush(&gs);
                sh_eprintf(s, "grep: %s: pattern too expensive to match\n", pattern);
                had_error = 1;
                break;
            }
            if (hit) {
                file_hit = 1;
                matched_any = 1;
                status = 0;
                count++;
                if (quiet) {
                    break;  /* -q: the first hit is the only answer it wants */
                }
                if (!counting && !only_files) {
                    grep_put(&gs, files > 1 ? targets[i] : NULL, numbering, lineno, line);
                }
            }
        }
        to_flush(&gs);
        free(src.rbuf);
        sh_target_close(fd);
        if (quiet) {
            if (file_hit) {
                free(line);
                sh_grep_free(re);
                return 0;
            }
            continue;
        }
        if (counting) {
            if (files > 1) {
                sh_printf(s, "%s:", targets[i]);
            }
            sh_printf(s, "%ld\n", count);
        } else if (only_files && file_hit) {
            sh_printf(s, "%s\n", targets[i]);
        }
    }
    free(line);
    sh_grep_free(re);
    if (quiet) {
        return 1;
    }
    if (had_error) {
        return 2;
    }
    return status;
}

/* ------------------------------------------------------------------ */
/* head and tail                                                        */
/* ------------------------------------------------------------------ */

/* head and tail stream through a sink in SH_HT_CHUNK pieces; window logic is in sh_headtail.c. */

/* Reads a source at an absolute offset for the backwards walk; stdin is buffered, not seeked. */
typedef struct {
    int fd;           /* >= 0 for a file, -1 for stdin */
    char *stream;     /* stdin's bytes, once pulled */
    size_t streamed;  /* how many of them */
} ht_src_t;

/* Cap on a buffered stream, since an unbounded pull would eat the heap every session shares. */
#define HT_STREAM_MAX (4 * 1024 * 1024)

static ssize_t ht_read_at(void *vctx, void *p, size_t n, size_t off)
{
    ht_src_t *src = vctx;
    size_t have = 0;

    if (src->fd < 0) {
        size_t left;

        if (off >= src->streamed) {
            return 0;
        }
        left = src->streamed - off;
        if (n > left) {
            n = left;
        }
        memcpy(p, src->stream + off, n);
        return (ssize_t)n;
    }
    if (lseek(src->fd, (off_t)off, SEEK_SET) == (off_t)-1) {
        return -1;
    }
    while (have < n) {
        ssize_t got = sh_target_read(src->fd, (char *)p + have, n - have);

        if (got <= 0) {
            break;
        }
        have += (size_t)got;
    }
    return (ssize_t)have;
}

/* Parses head/tail's count as a non-negative decimal; anything else is a usage error, not zero. */
static int ht_parse_count(sh_session_t *s, const char *cmd, const char *text,
                          size_t *out)
{
    unsigned long v = 0;
    const char *p;
    int digits = 0;

    for (p = text; *p != '\0'; p++) {
        if (*p < '0' || *p > '9') {
            sh_eprintf(s, "%s: %s: expected a non-negative integer\n", cmd, text);
            return -1;
        }
        if (v > (unsigned long)(SH_HT_SIZE_MAX / 10)) {
            sh_eprintf(s, "%s: %s: value too large (limit %d)\n", cmd, text, SH_HT_SIZE_MAX);
            return -1;
        }
        v = v * 10UL + (unsigned long)(*p - '0');
        digits = 1;
    }
    if (!digits) {
        sh_eprintf(s, "%s: %s: expected a non-negative integer\n", cmd, text);
        return -1;
    }
    *out = (size_t)v;
    return 0;
}

/* Reports one tool failure in the tool's own voice, per errno as strerror lacks sceNet codes. */
static void ht_report_error(sh_session_t *s, const char *cmd, const char *name)
{
    if (errno == E2BIG) {
        sh_eprintf(s, "%s: %s: line too long (limit %d bytes)\n", cmd, name,
                   SH_HT_LINE_MAX);
    } else if (errno == ENAMETOOLONG) {
        sh_eprintf(s, "%s: %s: result too large to print\n", cmd, name);
    } else if (errno == ERANGE) {
        sh_eprintf(s, "%s: %s: file too large (limit %d bytes)\n", cmd, name, SH_HT_SIZE_MAX);
    } else if (errno == ENOMEM) {
        sh_eprintf(s, "%s: %s: out of memory\n", cmd, name);
    } else if (errno == EIO) {
        sh_eprintf(s, "%s: %s: Input/output error\n", cmd, name);
    } else {
        sh_eprintf(s, "%s: %s: %s\n", cmd, name, strerror(errno));
    }
}

/* The sink: writes the answer out through sh_write, which always accepts what it is given. */
static int ht_emit(void *ctx, const char *data, size_t len)
{
    sh_write((sh_session_t *)ctx, data, len);
    return 0;
}

/* Pulls stdin into one bounded buffer for random access; over the bound it is refused. */
static int ht_slurp_stdin(sh_session_t *s, const char *cmd, ht_src_t *src)
{
    size_t used = 0;

    src->stream = malloc(HT_STREAM_MAX);
    if (src->stream == NULL) {
        sh_eprintf(s, "%s: out of memory\n", cmd);
        return -1;
    }
    while (!sh_session_interrupted(s)) {
        size_t n = sh_session_stdin_read(s, src->stream + used, HT_STREAM_MAX - used);

        if (n == 0) {
            break;
        }
        used += n;
        if (used == HT_STREAM_MAX) {
            sh_eprintf(s, "%s: %s: input larger than %d bytes\n", cmd,
                       "(standard input)", HT_STREAM_MAX);
            return -1;
        }
    }
    src->streamed = used;
    return 0;
}

/* One source through one of the tools, printed as it is produced. */
static int ht_run(sh_session_t *s, const char *cmd, int is_tail, int by_bytes,
                  size_t want, int fd, const char *name)
{
    ht_src_t src;
    char *buf;
    size_t len = 0;
    ssize_t got;
    int status = 0;

    buf = malloc(SH_HT_CHUNK);
    if (buf == NULL) {
        sh_eprintf(s, "%s: out of memory\n", cmd);
        return 1;
    }
    src.fd = fd;
    src.stream = NULL;
    src.streamed = 0;

    if (fd >= 0) {
        struct stat st;

        if (fstat(fd, &st) != 0) {
            sh_eprintf(s, "%s: %s: %s\n", cmd, name, strerror(errno));
            free(buf);
            return 1;
        }
        len = (size_t)st.st_size;
    } else if (ht_slurp_stdin(s, cmd, &src) != 0) {
        free(src.stream);
        free(buf);
        return 1;
    } else {
        len = src.streamed;
    }

    if (by_bytes) {
        /* A byte window is copied out a chunk at a time, never held whole. */
        size_t left = (want < len) ? want : len;
        size_t off = is_tail ? (len - left) : 0;

        while (left > 0) {
            size_t take = (left > SH_HT_CHUNK) ? SH_HT_CHUNK : left;
            ssize_t r = ht_read_at(&src, buf, take, off);

            if (r <= 0) {
                break;
            }
            sh_write(s, buf, (size_t)r);
            off += (size_t)r;
            left -= (size_t)r;
        }
        free(src.stream);
        free(buf);
        return 0;
    }

    got = is_tail ? sh_tail_lines(ht_read_at, &src, len, want, buf, SH_HT_CHUNK,
                                  ht_emit, s, NULL)
                  : sh_head_lines(ht_read_at, &src, want, buf, SH_HT_CHUNK, ht_emit, s,
                                  NULL, NULL);
    if (got < 0) {
        ht_report_error(s, cmd, name);
        status = 1;
    }
    free(src.stream);
    free(buf);
    return status;
    free(src.stream);
    free(buf);
    return status;
}

/* Shared driver for head and tail: same operands, stdin when no file; -f is tail only. */
static int ht_command(sh_session_t *s, const char *cmd, int is_tail, int argc,
                      char **argv)
{
    size_t want = 10;  /* both tools default to ten lines */
    int by_bytes = 0, follow = 0, i, bad = 0, files = 0;
    const char *targets[MAX_ARGS];

    for (i = 1; i < argc; i++) {
        const char *f = argv[i];

        if (is_tail && i == 1 && f[0] == '+') {
            /* tail +N is the legacy spelling of the count operand, only as the first argument. */
            if (ht_parse_count(s, cmd, f + 1, &want) != 0) {
                return 2;
            }
            continue;
        }
        if (f[0] != '-' || f[1] == '\0') {
            break;
        }
        if (strcmp(f, "--") == 0) {
            i++;
            break;
        }
        for (f = f + 1; *f != '\0'; f++) {
            const char *num;

            if (*f == 'f' && is_tail) {
                follow = 1;
                continue;
            }
            if (*f != 'n' && *f != 'c') {
                bad = *f;
                break;
            }
            by_bytes = (*f == 'c');
            /* -n10 and -c10 carry the count in the same argument; -n 10 takes the next. */
            num = (f[1] != '\0') ? f + 1 : ((++i < argc) ? argv[i] : NULL);
            if (num == NULL || ht_parse_count(s, cmd, num, &want) != 0) {
                if (num == NULL) {
                    sh_eprintf(s, "%s: option requires an argument -- '%c'\n", cmd, *f);
                }
                return 2;
            }
            if (f[1] != '\0') {
                f += strlen(f) - 1;
            }
        }
        if (bad != 0) {
            sh_eprintf(s, "%s: illegal option -- %c\n", cmd, bad);
            return 2;
        }
    }
    for (; i < argc && files < MAX_ARGS; i++) {
        targets[files++] = argv[i];
    }

    if (files == 0) {
        if (follow) {
            sh_eprintf(s, "%s: %s: cannot follow standard input\n", cmd,
                       "(standard input)");
            return 1;
        }
        return ht_run(s, cmd, is_tail, by_bytes, want, -1, "(standard input)");
    }

    {
        int status = 0;
        int k, headed = 0;

        /* Each file streams in turn, headed when several; an unopenable one fails the run. */
        for (k = 0; k < files; k++) {
            sh_target_t t;
            struct stat st;
            int fd, r;

            if (resolve_arg(s, cmd, targets[k], &t) != 0) {
                status = 1;
                continue;
            }
            if (t.kind == SH_TARGET_ROOT || t.kind == SH_TARGET_PSEUDO_DIR) {
                sh_eprintf(s, "%s: %s: Is a directory\n", cmd, targets[k]);
                status = 1;
                continue;
            }
            if (stat_target(&t, &st) == 0 && S_ISDIR(st.st_mode)) {
                sh_eprintf(s, "%s: %s: Is a directory\n", cmd, targets[k]);
                status = 1;
                continue;
            }
            fd = sh_target_open(&t);
            if (fd == -1) {
                sh_eprintf(s, "%s: %s: %s\n", cmd, targets[k], strerror(errno));
                status = 1;
                continue;
            }
            if (files > 1) {
                sh_printf(s, "%s==> %s <==\n", headed++ ? "\n" : "", targets[k]);
            }
            if (follow) {
                /* -f would park the single poll loop and every other session, so it is refused. */
                sh_eprintf(s, "%s: %s: following a file is not supported\n", cmd,
                           targets[k]);
                sh_target_close(fd);
                return 1;
            }
            r = ht_run(s, cmd, is_tail, by_bytes, want, fd, targets[k]);
            sh_target_close(fd);
            if (r != 0) {
                status = r;
            }
        }
        return status;
    }
}

static int cmd_head(sh_session_t *s, int argc, char **argv)
{
    return ht_command(s, "head", 0, argc, argv);
}

static int cmd_tail(sh_session_t *s, int argc, char **argv)
{
    return ht_command(s, "tail", 1, argc, argv);
}

/* ------------------------------------------------------------------ */
/* wc, sort, uniq                                                       */
/* ------------------------------------------------------------------ */

/* Counting, ordering and collapsing live in sh_textops.c; this file holds source, sink, columns. */

/* A source is one open file (fd) or stdin pulled into a bounded buffer for random access. */
typedef struct {
    int fd;         /* >= 0 for a file, -1 for the buffer */
    char *buf;      /* the bytes: stdin's copy, or a file read into this */
    size_t len;     /* how many are valid */
    size_t off;     /* how far the fill has handed out */
} to_src_t;

static ssize_t to_fill(void *vctx, char *p, size_t want)
{
    to_src_t *src = vctx;
    size_t left;

    if (src->off >= src->len) {
        return 0;
    }
    left = src->len - src->off;
    if (want > left) {
        want = left;
    }
    memcpy(p, src->buf + src->off, want);
    src->off += want;
    return (ssize_t)want;
}

/* Reads a whole file into a fresh buffer, refusing anything past the bound, not truncating. */
static int to_load_fd(int fd, char **out, size_t *out_len)
{
    char *b;
    size_t used = 0;

    b = malloc(SH_TEXTOPS_BYTES_MAX);
    if (b == NULL) {
        errno = ENOMEM;
        return -1;
    }
    for (;;) {
        ssize_t n = read(fd, b + used, SH_TEXTOPS_BYTES_MAX - used);

        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            free(b);
            return -1;
        }
        if (n == 0) {
            break;
        }
        used += (size_t)n;
        if (used == SH_TEXTOPS_BYTES_MAX) {
            free(b);
            errno = EFBIG;
            return -1;
        }
    }
    *out = b;
    *out_len = used;
    return 0;
}

/* Pulls stdin into a fresh buffer, bounded and interruptible like head and tail. */
static int to_load_stdin(sh_session_t *s, const char *cmd, to_src_t *src)
{
    src->fd = -1;
    src->off = 0;
    src->buf = malloc(SH_TEXTOPS_BYTES_MAX);
    if (src->buf == NULL) {
        sh_eprintf(s, "%s: out of memory\n", cmd);
        return -1;
    }
    src->len = 0;
    while (!sh_session_interrupted(s)) {
        size_t n = sh_session_stdin_read(s, src->buf + src->len,
                                         SH_TEXTOPS_BYTES_MAX - src->len);

        if (n == 0) {
            break;
        }
        src->len += n;
        if (src->len == SH_TEXTOPS_BYTES_MAX) {
            sh_eprintf(s, "%s: (standard input): input larger than %d bytes\n", cmd,
                       SH_TEXTOPS_BYTES_MAX);
            free(src->buf);
            src->buf = NULL;
            return -1;
        }
    }
    return 0;
}

/* Names the tool's verdict in its own voice, per errno as strerror lacks sceNet codes. */
static void to_report_error(sh_session_t *s, const char *cmd, const char *name)
{
    if (errno == E2BIG) {
        sh_eprintf(s, "%s: %s: line too long (limit %d bytes)\n", cmd, name,
                   SH_TEXTOPS_LINE_MAX);
    } else if (errno == EFBIG && strcmp(cmd, "wc") == 0) {
        sh_eprintf(s, "%s: %s: file too large (limit %d bytes)\n", cmd, name,
                   SH_TEXTOPS_BYTES_MAX);
    } else if (errno == EFBIG) {
        sh_eprintf(s, "%s: %s: input too large (limit %d bytes or %d lines)\n", cmd, name,
                   SH_TEXTOPS_BYTES_MAX, SH_TEXTOPS_LINES_MAX);
    } else if (errno == EILSEQ) {
        sh_eprintf(s, "%s: %s: numeric field too long (limit %d digits)\n", cmd, name,
                   SH_TEXTOPS_DIGITS_MAX);
    } else if (errno == ENOMEM) {
        sh_eprintf(s, "%s: %s: out of memory\n", cmd, name);
    } else if (errno == EIO) {
        sh_eprintf(s, "%s: %s: Input/output error\n", cmd, name);
    } else {
        sh_eprintf(s, "%s: %s: %s\n", cmd, name, strerror(errno));
    }
}

static int to_line(void *vctx, const char *line, size_t len, unsigned long count)
{
    to_sink_t *sink = vctx;

    if (sink->with_count) {
        char head[16];
        int n = snprintf(head, sizeof(head), "%7lu ", count);

        if (n > 0) {
            to_put(sink, head, (size_t)n);
        }
    }
    to_put(sink, line, len);
    to_put(sink, "\n", 1);
    return 0;
}

static int cmd_wc(sh_session_t *s, int argc, char **argv)
{
    int want_l = 0, want_w = 0, want_b = 0, i, files = 0, status = 0, k;
    const char *names[MAX_ARGS];
    sh_wc_t counts[MAX_ARGS];
    int have[MAX_ARGS];
    unsigned long total_l = 0, total_w = 0, total_b = 0;
    unsigned int width;

    for (i = 1; i < argc && argv[i][0] == '-' && argv[i][1] != '\0'; i++) {
        const char *o = argv[i] + 1;

        if (strcmp(o, "-") == 0) {
            break; /* a bare -- ends the flags */
        }
        for (; *o; o++) {
            switch (*o) {
            case 'l':
                want_l = 1;
                break;
            case 'w':
                want_w = 1;
                break;
            case 'c':
                want_b = 1;
                break;
            default:
                sh_eprintf(s, "wc: illegal option -- %c\n", *o);
                return 2;
            }
        }
    }
    if (!want_l && !want_w && !want_b) {
        want_l = want_w = want_b = 1; /* no flag means all three, as wc does */
    }
    for (; i < argc && files < MAX_ARGS; i++) {
        names[files++] = argv[i];
    }

    memset(counts, 0, sizeof(counts));
    memset(have, 0, sizeof(have));

    if (files == 0) {
        to_src_t src;
        sh_wc_t wc;

        memset(&wc, 0, sizeof(wc));
        wc.want_lines = want_l;
        wc.want_words = want_w;
        wc.want_bytes = want_b;
        if (to_load_stdin(s, "wc", &src) != 0) {
            return 1;
        }
        src.off = 0;
        if (sh_wc(to_fill, &src, &wc) != 0) {
            to_report_error(s, "wc", "(standard input)");
            free(src.buf);
            return 1;
        }
        free(src.buf);
        /* No name to align against: a bare count for one column, else the digits of the bytes. */
        {
            unsigned long v = wc.bytes;
            int w = 1;

            while (v >= 10) {
                v /= 10;
                w++;
            }
            if (want_l + want_w + want_b == 1) {
                w = 1;
            }
            if (want_l) {
                sh_printf(s, "%*lu", w, wc.lines);
            }
            if (want_w) {
                sh_printf(s, "%s%*lu", want_l ? " " : "", w, wc.words);
            }
            if (want_b) {
                sh_printf(s, "%s%*lu", (want_l || want_w) ? " " : "", w, wc.bytes);
            }
        }
        sh_printf(s, "\n");
        return 0;
    }

    for (k = 0; k < files; k++) {
        sh_target_t t;
        struct stat st;
        int fd;
        to_src_t src;

        if (resolve_arg(s, "wc", names[k], &t) != 0) {
            status = 1;
            continue;
        }
        if (stat_target(&t, &st) == 0 && S_ISDIR(st.st_mode)) {
            sh_eprintf(s, "wc: %s: Is a directory\n", names[k]);
            status = 1;
            continue;
        }
        fd = sh_target_open(&t);
        if (fd == -1) {
            sh_eprintf(s, "wc: %s: %s\n", names[k], strerror(errno));
            status = 1;
            continue;
        }
        if (to_load_fd(fd, &src.buf, &src.len) != 0) {
            to_report_error(s, "wc", names[k]);
            sh_target_close(fd);
            status = 1;
            continue;
        }
        sh_target_close(fd);
        src.fd = -1;
        src.off = 0;
        memset(&counts[k], 0, sizeof(sh_wc_t));
        counts[k].want_lines = want_l;
        counts[k].want_words = want_w;
        counts[k].want_bytes = want_b;
        if (sh_wc(to_fill, &src, &counts[k]) != 0) {
            to_report_error(s, "wc", names[k]);
            free(src.buf);
            status = 1;
            continue;
        }
        free(src.buf);
        have[k] = 1;
        total_l += counts[k].lines;
        total_w += counts[k].words;
        total_b += counts[k].bytes;
    }

    /* GNU's width: the digits of the summed byte size, and none for one column of one input. */
    {
        unsigned long v = total_b;

        width = 1;
        while (v >= 10) {
            v /= 10;
            width++;
        }
        if (files == 1 && want_l + want_w + want_b == 1) {
            width = 1;
        }
    }

    for (k = 0; k < files; k++) {
        char row[SH_POSIX_PATH_MAX + 64];
        int n = 0;

        if (!have[k]) {
            continue;
        }
        if (want_l) {
            n += snprintf(row + n, sizeof(row) - n, "%*lu ", width, counts[k].lines);
        }
        if (want_w) {
            n += snprintf(row + n, sizeof(row) - n, "%*lu ", width, counts[k].words);
        }
        if (want_b) {
            n += snprintf(row + n, sizeof(row) - n, "%*lu ", width, counts[k].bytes);
        }
        snprintf(row + n, sizeof(row) - n, "%s", names[k]);
        sh_printf(s, "%s\n", row);
    }
    if (files > 1) {
        char row[64];
        int n = 0;

        if (want_l) {
            n += snprintf(row + n, sizeof(row) - n, "%*lu ", width, total_l);
        }
        if (want_w) {
            n += snprintf(row + n, sizeof(row) - n, "%*lu ", width, total_w);
        }
        if (want_b) {
            n += snprintf(row + n, sizeof(row) - n, "%*lu ", width, total_b);
        }
        snprintf(row + n, sizeof(row) - n, "total");
        sh_printf(s, "%s\n", row);
    }
    return status;
}

/* Runs one tool over one source: pull the bytes, hand lines to the sink; unreadable is an error. */
static int to_run(sh_session_t *s, const char *cmd, int is_sort, int numeric, int reverse,
                  int uniq_or_count, int dups, int singles, to_src_t *src,
                  const char *name)
{
    to_sink_t sink;
    int r;

    sink.s = s;
    sink.with_count = is_sort ? 0 : uniq_or_count;
    sink.used = 0;
    src->off = 0;
    if (is_sort) {
        r = sh_sort(to_fill, src, src->len, numeric, reverse, uniq_or_count, to_line, &sink);
    } else {
        r = sh_uniq(to_fill, src, src->len, uniq_or_count, dups, singles, to_line, &sink);
    }
    to_flush(&sink);
    if (r != 0) {
        to_report_error(s, cmd, name);
        return 1;
    }
    return 0;
}

static int cmd_sort(sh_session_t *s, int argc, char **argv)
{
    int numeric = 0, reverse = 0, uniq = 0, i, files = 0, status = 0, k;
    const char *names[MAX_ARGS];

    for (i = 1; i < argc && argv[i][0] == '-' && argv[i][1] != '\0'; i++) {
        const char *o = argv[i] + 1;

        if (strcmp(o, "-") == 0) {
            break;
        }
        for (; *o; o++) {
            if (*o == 'n') {
                numeric = 1;
            } else if (*o == 'r') {
                reverse = 1;
            } else if (*o == 'u') {
                uniq = 1;
            } else {
                sh_eprintf(s, "sort: illegal option -- %c\n", *o);
                return 2;
            }
        }
    }
    for (; i < argc && files < MAX_ARGS; i++) {
        names[files++] = argv[i];
    }

    if (files == 0) {
        to_src_t src;
        int r;

        if (to_load_stdin(s, "sort", &src) != 0) {
            return 1;
        }
        r = to_run(s, "sort", 1, numeric, reverse, uniq, 0, 0, &src, "(standard input)");
        free(src.buf);
        return r;
    }
    for (k = 0; k < files; k++) {
        sh_target_t t;
        struct stat st;
        int fd;
        to_src_t src;

        if (resolve_arg(s, "sort", names[k], &t) != 0) {
            status = 1;
            continue;
        }
        if (stat_target(&t, &st) == 0 && S_ISDIR(st.st_mode)) {
            sh_eprintf(s, "sort: %s: Is a directory\n", names[k]);
            status = 1;
            continue;
        }
        fd = sh_target_open(&t);
        if (fd == -1) {
            sh_eprintf(s, "sort: %s: %s\n", names[k], strerror(errno));
            status = 1;
            continue;
        }
        if (to_load_fd(fd, &src.buf, &src.len) != 0) {
            to_report_error(s, "sort", names[k]);
            sh_target_close(fd);
            status = 1;
            continue;
        }
        sh_target_close(fd);
        src.fd = -1;
        if (to_run(s, "sort", 1, numeric, reverse, uniq, 0, 0, &src, names[k]) != 0) {
            status = 1;
        }
        free(src.buf);
    }
    return status;
}

static int cmd_uniq(sh_session_t *s, int argc, char **argv)
{
    int show_count = 0, dups = 0, singles = 0, i, files = 0, status = 0;
    const char *names[MAX_ARGS];
    to_src_t src_in;

    for (i = 1; i < argc && argv[i][0] == '-' && argv[i][1] != '\0'; i++) {
        const char *o = argv[i] + 1;

        if (strcmp(o, "-") == 0) {
            break;
        }
        for (; *o; o++) {
            if (*o == 'c') {
                show_count = 1;
            } else if (*o == 'd') {
                dups = 1;
            } else if (*o == 'u') {
                singles = 1;
            } else {
                sh_eprintf(s, "uniq: illegal option -- %c\n", *o);
                return 2;
            }
        }
    }
    for (; i < argc && files < MAX_ARGS; i++) {
        names[files++] = argv[i];
    }

    /* uniq reads at most one file; a second name is coreutils' usage error. */
    if (files > 1) {
        sh_eprintf(s, "uniq: extra operand '%s'\n", names[1]);
        return 2;
    }

    memset(&src_in, 0, sizeof(src_in));
    if (files == 0) {
        if (to_load_stdin(s, "uniq", &src_in) != 0) {
            return 1;
        }
        status = to_run(s, "uniq", 0, 0, 0, show_count, dups, singles, &src_in,
                        "(standard input)");
        free(src_in.buf);
        return status;
    }
    {
        sh_target_t t;
        struct stat st;
        int fd;
        to_src_t src;

        if (resolve_arg(s, "uniq", names[0], &t) != 0) {
            return 1;
        }
        if (stat_target(&t, &st) == 0 && S_ISDIR(st.st_mode)) {
            sh_eprintf(s, "uniq: %s: Is a directory\n", names[0]);
            return 1;
        }
        fd = sh_target_open(&t);
        if (fd == -1) {
            sh_eprintf(s, "uniq: %s: %s\n", names[0], strerror(errno));
            return 1;
        }
        if (to_load_fd(fd, &src.buf, &src.len) != 0) {
            to_report_error(s, "uniq", names[0]);
            sh_target_close(fd);
            return 1;
        }
        sh_target_close(fd);
        src.fd = -1;
        status = to_run(s, "uniq", 0, 0, 0, show_count, dups, singles, &src, names[0]);
        free(src.buf);
    }
    return status;
}

static int cmd_echo(sh_session_t *s, int argc, char **argv)
{
    int i, nl = 1, start = 1;

    if (argc > 1 && strcmp(argv[1], "-n") == 0) {
        nl = 0;
        start = 2;
    }
    for (i = start; i < argc; i++) {
        sh_printf(s, "%s%s", (i > start) ? " " : "", argv[i]);
    }
    if (nl) {
        sh_printf(s, "\n");
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* creation and removal                                                 */
/* ------------------------------------------------------------------ */

static int cmd_touch(sh_session_t *s, int argc, char **argv)
{
    int i, status = 0;

    if (argc < 2) {
        sh_eprintf(s, "touch: missing file operand\n");
        return 1;
    }
    for (i = 1; i < argc; i++) {
        sh_target_t t;
        struct timeval times[2];
        struct stat st;
        int fd;

        if (resolve_arg(s, "touch", argv[i], &t) != 0 ||
            check_writable(s, "touch", argv[i], &t) != 0) {
            status = 1;
            continue;
        }
        times[0].tv_sec = times[1].tv_sec = time(NULL);
        times[0].tv_usec = times[1].tv_usec = 0;
        if (stat_target(&t, &st) == 0) {
            if (utimes(t.sce, times) != 0) {
                sh_eprintf(s, "touch: %s: %s\n", argv[i], strerror(errno));
                status = 1;
            }
            continue;
        }
        fd = open(t.sce, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) {
            sh_eprintf(s, "touch: %s: %s\n", argv[i], strerror(errno));
            status = 1;
            continue;
        }
        close(fd);
    }
    return status;
}

/* mkdir -p: walks the leaf's already-normalized POSIX path one component at a
 * time, so every intermediate is created by the same path layer as the leaf. */
static int make_dirs(sh_session_t *s, const char *cmd, const sh_target_t *leaf)
{
    char so_far[SH_POSIX_PATH_MAX];
    size_t len;
    const char *p;

    if (leaf->kind == SH_TARGET_ROOT || leaf->dev == NULL) {
        sh_eprintf(s, "%s: %s: Read-only file system\n", cmd, leaf->posix);
        return -1;
    }
    /* Start below the device root: it exists already and nothing may create it. */
    snprintf(so_far, sizeof(so_far), "%s", leaf->dev->posix);
    len = strlen(so_far);
    p = leaf->posix + strlen(leaf->dev->posix);
    while (*p == '/') {
        p++;
    }
    while (*p != '\0') {
        sh_target_t t;
        sh_fsctx_t fs;
        const char *slash;
        size_t keep;

        slash = strchr(p, '/');
        keep = (slash != NULL) ? (size_t)(slash - p) : strlen(p);
        if (keep == 0) {
            p++;
            continue;
        }
        if (len + 1 + keep + 1 > sizeof(so_far)) {
            errno = ENAMETOOLONG;
            sh_eprintf(s, "%s: %s: %s\n", cmd, leaf->posix, strerror(errno));
            return -1;
        }
        so_far[len++] = '/';
        memcpy(so_far + len, p, keep);
        len += keep;
        so_far[len] = '\0';
        p += keep;
        while (*p == '/') {
            p++;
        }

        fs.cwd = so_far;
        fs.home = so_far;
        if (sh_path_resolve(&fs, NULL, &t) != 0) {
            sh_eprintf(s, "%s: %s: %s\n", cmd, so_far, strerror(errno));
            return -1;
        }
        if (mkdir(t.sce, 0755) != 0 && errno != EEXIST) {
            sh_eprintf(s, "%s: %s: %s\n", cmd, so_far, strerror(errno));
            return -1;
        }
    }
    return 0;
}

static int cmd_mkdir(sh_session_t *s, int argc, char **argv)
{
    const char *targets[MAX_ARGS];
    int i, n, status = 0, bad = 0, parents = 0;

    for (i = 1; i < argc && argv[i][0] == '-' && argv[i][1] != '\0'; i++) {
        const char *f;

        for (f = argv[i] + 1; *f != '\0'; f++) {
            if (*f == 'p') {
                parents = 1;
            } else {
                bad = *f;
            }
        }
    }
    if (bad != 0) {
        sh_eprintf(s, "mkdir: illegal option -- %c\n", bad);
        return 1;
    }
    n = 0;
    for (; i < argc; i++) {
        if (n < MAX_ARGS) {
            targets[n++] = argv[i];
        }
    }
    if (n == 0) {
        sh_eprintf(s, "mkdir: missing operand\n");
        return 1;
    }
    for (i = 0; i < n; i++) {
        sh_target_t t;

        if (resolve_arg(s, "mkdir", targets[i], &t) != 0 ||
            check_writable(s, "mkdir", targets[i], &t) != 0) {
            status = 1;
            continue;
        }
        if (parents) {
            if (make_dirs(s, "mkdir", &t) != 0) {
                status = 1;
            }
            continue;
        }
        if (mkdir(t.sce, 0755) != 0) {
            sh_eprintf(s, "mkdir: %s: %s\n", targets[i], strerror(errno));
            status = 1;
        }
    }
    return status;
}

static int remove_tree(sh_session_t *s, const char *cmd, const sh_target_t *t,
                       int top_level, int depth, sh_walk_t *w)
{
    DIR *dir;
    struct dirent *ent;
    int failed = 0;

    if (top_level && t->kind == SH_TARGET_DEVICE) {
        sh_eprintf(s, "%s: %s: Cannot remove a device root\n", cmd,
                   t->posix);
        return 1;
    }
    if (depth > MAX_DEPTH) {
        sh_eprintf(s, "%s: %s: Too many levels of directories\n", cmd,
                   t->posix);
        return 1;
    }
    dir = opendir(t->sce);
    if (dir == NULL) {
        sh_eprintf(s, "%s: %s: %s\n", cmd, t->posix, strerror(errno));
        return 1;
    }
    while ((ent = readdir(dir)) != NULL) {
        struct stat st;

        sh_session_yield(s);
        if (sh_session_interrupted(s)) {
            break;
        }
        if (sh_is_dot(ent->d_name)) {
            continue;
        }
        if (sh_join(w->a, sizeof(w->a), t->sce, ent->d_name) != 0 ||
            sh_join(w->pa, sizeof(w->pa), t->posix, ent->d_name) != 0) {
            failed = 1;
            continue;
        }
        if (stat(w->a, &st) != 0) {
            sh_eprintf(s, "%s: %s: %s\n", cmd, w->pa, strerror(errno));
            failed = 1;
            continue;
        }
        if (!S_ISDIR(st.st_mode)) {
            if (unlink(w->a) != 0) {
                sh_eprintf(s, "%s: %s: %s\n", cmd, w->pa, strerror(errno));
                failed = 1;
            }
            continue;
        }
        {
            sh_fsctx_t fs;
            sh_target_t child;

            fs.cwd = t->posix;
            fs.home = t->posix;
            if (sh_path_resolve(&fs, ent->d_name, &child) != 0) {
                failed = 1;
                continue;
            }
            if (remove_tree(s, cmd, &child, 0, depth + 1, w) != 0) {
                failed = 1;
            }
        }
    }
    closedir(dir);
    if (rmdir(t->sce) != 0) {
        sh_eprintf(s, "%s: %s: %s\n", cmd, t->posix, strerror(errno));
        failed = 1;
    }
    return failed ? 1 : 0;
}

static int cmd_rm(sh_session_t *s, int argc, char **argv)
{
    const char *targets[MAX_ARGS];
    int i, n, status = 0, bad = 0, recursive = 0, force = 0;

    for (i = 1; i < argc && argv[i][0] == '-' && argv[i][1] != '\0'; i++) {
        const char *f;

        for (f = argv[i] + 1; *f != '\0'; f++) {
            if (*f == 'r' || *f == 'R') {
                recursive = 1;
            } else if (*f == 'f') {
                force = 1;
            } else {
                bad = *f;
            }
        }
    }
    if (bad != 0) {
        sh_eprintf(s, "rm: illegal option -- %c\n", bad);
        return 1;
    }
    n = 0;
    for (; i < argc; i++) {
        if (n < MAX_ARGS) {
            targets[n++] = argv[i];
        }
    }
    if (n == 0) {
        if (!force) {
            sh_eprintf(s, "rm: missing operand\n");
            return 1;
        }
        return 0;
    }
    for (i = 0; i < n; i++) {
        sh_target_t t;
        struct stat st;

        if (resolve_arg(s, "rm", targets[i], &t) != 0 ||
            check_writable(s, "rm", targets[i], &t) != 0) {
            if (!force) {
                status = 1;
            }
            continue;
        }
        if (t.kind == SH_TARGET_ROOT) {
            if (!force) {
                sh_eprintf(s, "rm: %s: Invalid argument\n", targets[i]);
            }
            status = 1;
            continue;
        }
        if (stat_target(&t, &st) != 0) {
            if (!force) {
                sh_eprintf(s, "rm: %s: %s\n", targets[i], strerror(errno));
                status = 1;
            }
            continue;
        }
        if (!S_ISDIR(st.st_mode)) {
            if (unlink(t.sce) != 0 && !force) {
                sh_eprintf(s, "rm: %s: %s\n", targets[i], strerror(errno));
                status = 1;
            }
            continue;
        }
        if (!recursive) {
            sh_eprintf(s, "rm: %s: Is a directory\n", targets[i]);
            status = 1;
            continue;
        }
        /* Judged on the argument as typed: resolving "dir/." already yields "dir". */
        if (sh_is_dot(sh_basename(targets[i]))) {
            sh_eprintf(s, "rm: %s: Invalid argument\n", targets[i]);
            status = 1;
            continue;
        }
        {
            sh_walk_t *w = malloc(sizeof(*w));

            if (w == NULL) {
                sh_eprintf(s, "rm: out of memory\n");
                status = 1;
                continue;
            }
            if (remove_tree(s, "rm", &t, 1, 0, w) != 0) {
                status = 1;
            }
            free(w);
        }
    }
    return status;
}

/* rmdir(1) proper: empty directories only. Recursion is rm -r's job. */
static int cmd_rmdir(sh_session_t *s, int argc, char **argv)
{
    const char *targets[MAX_ARGS];
    int i, n, status = 0;

    n = collect_targets(argv, argc, NULL, targets, MAX_ARGS, NULL);
    if (n == 0) {
        sh_eprintf(s, "rmdir: missing operand\n");
        return 1;
    }
    for (i = 0; i < n; i++) {
        sh_target_t t;
        struct stat st;

        if (resolve_arg(s, "rmdir", targets[i], &t) != 0 ||
            check_writable(s, "rmdir", targets[i], &t) != 0) {
            status = 1;
            continue;
        }
        if (t.kind != SH_TARGET_PATH) {
            sh_eprintf(s, "rmdir: %s: Invalid argument\n", targets[i]);
            status = 1;
            continue;
        }
        if (stat_target(&t, &st) != 0) {
            sh_eprintf(s, "rmdir: %s: %s\n", targets[i], strerror(errno));
            status = 1;
            continue;
        }
        if (!S_ISDIR(st.st_mode)) {
            sh_eprintf(s, "rmdir: %s: Not a directory\n", targets[i]);
            status = 1;
            continue;
        }
        if (rmdir(t.sce) != 0) {
            sh_eprintf(s, "rmdir: %s: %s\n", targets[i], strerror(errno));
            status = 1;
        }
    }
    return status;
}

/* ------------------------------------------------------------------ */
/* cp, mv                                                               */
/* ------------------------------------------------------------------ */

/* src and dst are sceIo paths for the calls; src_name and dst_name are the
 * POSIX spellings the user typed or would recognize, for errors. cmd names the
 * command speaking, since mv copies across devices too. */
static int copy_file(sh_session_t *s, const char *cmd, const char *src,
                     const char *src_name, const char *dst,
                     const char *dst_name, char *buf)
{
    ssize_t got;
    int in, out;

    in = open(src, O_RDONLY);
    if (in < 0) {
        sh_eprintf(s, "%s: %s: %s\n", cmd, src_name, strerror(errno));
        return -1;
    }
    out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out < 0) {
        int saved = errno;

        close(in);
        sh_eprintf(s, "%s: %s: %s\n", cmd, dst_name, strerror(saved));
        return -1;
    }
    while ((got = read(in, buf, SSHD_COPY_BUFSIZE)) > 0) {
        ssize_t put = 0;

        sh_session_yield(s);
        while (put < got) {
            ssize_t one = write(out, buf + put, (size_t)(got - put));

            if (one <= 0) {
                close(in);
                close(out);
                sh_eprintf(s, "%s: %s: %s\n", cmd, dst_name, strerror(errno));
                return -1;
            }
            put += one;
        }
    }
    close(in);
    close(out);
    if (got < 0) {
        sh_eprintf(s, "%s: %s: %s\n", cmd, src_name, strerror(errno));
        return -1;
    }
    return 0;
}

static int copy_tree(sh_session_t *s, const char *cmd, const sh_target_t *src,
                     const sh_target_t *dst, int depth, sh_walk_t *w, char *buf)
{
    DIR *dir;
    struct dirent *ent;
    int failed = 0;

    if (depth > MAX_DEPTH) {
        sh_eprintf(s, "%s: %s: Too many levels of directories\n", cmd,
                   src->posix);
        return -1;
    }
    if (mkdir(dst->sce, 0755) != 0 && errno != EEXIST) {
        sh_eprintf(s, "%s: %s: %s\n", cmd, dst->posix, strerror(errno));
        return -1;
    }
    dir = opendir(src->sce);
    if (dir == NULL) {
        sh_eprintf(s, "%s: %s: %s\n", cmd, src->posix, strerror(errno));
        return -1;
    }
    while ((ent = readdir(dir)) != NULL) {
        struct stat st;

        sh_session_yield(s);
        if (sh_session_interrupted(s)) {
            break;
        }
        if (sh_is_dot(ent->d_name)) {
            continue;
        }
        if (sh_join(w->a, sizeof(w->a), src->sce, ent->d_name) != 0 ||
            sh_join(w->b, sizeof(w->b), dst->sce, ent->d_name) != 0 ||
            sh_join(w->pa, sizeof(w->pa), src->posix, ent->d_name) != 0 ||
            sh_join(w->pb, sizeof(w->pb), dst->posix, ent->d_name) != 0) {
            failed = 1;
            continue;
        }
        if (stat(w->a, &st) != 0) {
            sh_eprintf(s, "%s: %s: %s\n", cmd, w->pa, strerror(errno));
            failed = 1;
            continue;
        }
        if (!S_ISDIR(st.st_mode)) {
            if (copy_file(s, cmd, w->a, w->pa, w->b, w->pb, buf) != 0) {
                failed = 1;
            }
            continue;
        }
        {
            sh_fsctx_t fs;
            sh_target_t child_src, child_dst;

            fs.cwd = src->posix;
            fs.home = src->posix;
            if (sh_path_resolve(&fs, ent->d_name, &child_src) != 0) {
                failed = 1;
                continue;
            }
            fs.cwd = dst->posix;
            fs.home = dst->posix;
            if (sh_path_resolve(&fs, ent->d_name, &child_dst) != 0) {
                failed = 1;
                continue;
            }
            if (copy_tree(s, cmd, &child_src, &child_dst, depth + 1, w,
                          buf) != 0) {
                failed = 1;
            }
        }
    }
    closedir(dir);
    return failed ? 1 : 0;
}

/* `mv a dir` and `cp a dir` land inside dir. Rewrites dst in place. */
static void land_in_dir(sh_target_t *dst, const sh_target_t *src)
{
    char sce[SH_SCE_PATH_MAX], posix[SH_POSIX_PATH_MAX];
    struct stat st;

    if (dst->kind == SH_TARGET_ROOT || dst->kind == SH_TARGET_PSEUDO ||
        dst->kind == SH_TARGET_PSEUDO_DIR ||
        stat(dst->sce, &st) != 0 || !S_ISDIR(st.st_mode)) {
        return;
    }
    if (sh_join(sce, sizeof(sce), dst->sce, sh_basename(src->posix)) != 0 ||
        sh_join(posix, sizeof(posix), dst->posix,
                sh_basename(src->posix)) != 0) {
        return;
    }
    snprintf(dst->sce, sizeof(dst->sce), "%s", sce);
    snprintf(dst->posix, sizeof(dst->posix), "%s", posix);
    dst->kind = SH_TARGET_PATH;
}

static int cmd_cp(sh_session_t *s, int argc, char **argv)
{
    const char *targets[MAX_ARGS];
    int i, n, status = 0, bad = 0, recursive = 0;
    char *buf;

    for (i = 1; i < argc && argv[i][0] == '-' && argv[i][1] != '\0'; i++) {
        const char *f;

        for (f = argv[i] + 1; *f != '\0'; f++) {
            if (*f == 'r' || *f == 'R') {
                recursive = 1;
            } else {
                bad = *f;
            }
        }
    }
    if (bad != 0) {
        sh_eprintf(s, "cp: illegal option -- %c\n", bad);
        return 1;
    }
    n = 0;
    for (; i < argc; i++) {
        if (n < MAX_ARGS) {
            targets[n++] = argv[i];
        }
    }
    if (n < 2) {
        sh_eprintf(s, "cp: missing destination file operand\n");
        return 1;
    }
    buf = malloc(SSHD_COPY_BUFSIZE);
    if (buf == NULL) {
        sh_eprintf(s, "cp: out of memory\n");
        return 1;
    }
    for (i = 0; i < n - 1; i++) {
        sh_target_t src, dst;
        struct stat st;

        /* Both sides are resolved per source: reusing one dst across several
         * sources would keep the first source's name after land_in_dir. */
        if (resolve_arg(s, "cp", targets[i], &src) != 0 ||
            resolve_arg(s, "cp", targets[n - 1], &dst) != 0 ||
            check_writable(s, "cp", targets[n - 1], &dst) != 0) {
            status = 1;
            continue;
        }
        if (src.kind == SH_TARGET_ROOT) {
            sh_eprintf(s, "cp: %s: Invalid argument\n", targets[i]);
            status = 1;
            continue;
        }
        if (stat_target(&src, &st) != 0) {
            sh_eprintf(s, "cp: %s: %s\n", targets[i], strerror(errno));
            status = 1;
            continue;
        }
        land_in_dir(&dst, &src);
        if (S_ISDIR(st.st_mode)) {
            sh_walk_t *w = malloc(sizeof(*w));

            if (!recursive) {
                sh_eprintf(s, "cp: %s: Is a directory\n", targets[i]);
                status = 1;
                continue;
            }
            if (w == NULL) {
                sh_eprintf(s, "cp: out of memory\n");
                status = 1;
                continue;
            }
            if (copy_tree(s, "cp", &src, &dst, 0, w, buf) != 0) {
                status = 1;
            }
            free(w);
            continue;
        }
        if (copy_file(s, "cp", src.sce, src.posix, dst.sce, dst.posix,
                      buf) != 0) {
            status = 1;
        }
    }
    free(buf);
    return status;
}

static int cmd_mv(sh_session_t *s, int argc, char **argv)
{
    const char *targets[MAX_ARGS];
    int i, n, status = 0;

    n = collect_targets(argv, argc, NULL, targets, MAX_ARGS, NULL);
    if (n < 2) {
        sh_eprintf(s, "mv: missing destination file operand\n");
        return 1;
    }
    for (i = 0; i < n - 1; i++) {
        sh_target_t src, dst;

        if (resolve_arg(s, "mv", targets[i], &src) != 0 ||
            resolve_arg(s, "mv", targets[n - 1], &dst) != 0) {
            status = 1;
            continue;
        }
        if (src.kind == SH_TARGET_ROOT || dst.kind == SH_TARGET_ROOT) {
            sh_eprintf(s, "mv: %s: Invalid argument\n",
                       (src.kind == SH_TARGET_ROOT) ? targets[i]
                                                    : targets[n - 1]);
            status = 1;
            continue;
        }
        if (check_writable(s, "mv", targets[n - 1], &dst) != 0) {
            status = 1;
            continue;
        }
        land_in_dir(&dst, &src);

        /* sceIoRename cannot cross devices, so check before trying it and fall
         * back to copy-then-delete rather than reporting something the user has
         * to do by hand. */
        if (src.dev == dst.dev) {
            if (rename(src.sce, dst.sce) == 0) {
                continue;
            }
            if (errno == EEXIST && unlink(dst.sce) == 0 &&
                rename(src.sce, dst.sce) == 0) {
                continue;
            }
            sh_eprintf(s, "mv: %s -> %s: %s\n", src.posix, dst.posix,
                       strerror(errno));
            status = 1;
            continue;
        }
        {
            struct stat st;
            char *buf = malloc(SSHD_COPY_BUFSIZE);
            sh_walk_t *w;

            if (buf == NULL) {
                sh_eprintf(s, "mv: out of memory\n");
                status = 1;
                continue;
            }
            if (stat_target(&src, &st) != 0) {
                sh_eprintf(s, "mv: %s: %s\n", targets[i], strerror(errno));
                status = 1;
                free(buf);
                continue;
            }
            if (!S_ISDIR(st.st_mode)) {
                if (copy_file(s, "mv", src.sce, src.posix, dst.sce, dst.posix,
                              buf) != 0) {
                    status = 1;
                } else if (unlink(src.sce) != 0) {
                    sh_eprintf(s, "mv: %s: %s\n", src.posix, strerror(errno));
                    status = 1;
                }
                free(buf);
                continue;
            }
            w = malloc(sizeof(*w));
            if (w == NULL) {
                sh_eprintf(s, "mv: out of memory\n");
                status = 1;
                free(buf);
                continue;
            }
            if (copy_tree(s, "mv", &src, &dst, 0, w, buf) != 0 ||
                remove_tree(s, "mv", &src, 1, 0, w) != 0) {
                status = 1;
            }
            free(w);
            free(buf);
        }
    }
    return status;
}

/* ------------------------------------------------------------------ */
/* stat, df, du                                                         */
/* ------------------------------------------------------------------ */

static int cmd_stat(sh_session_t *s, int argc, char **argv)
{
    int i, status = 0;

    if (argc < 2) {
        sh_eprintf(s, "stat: missing operand\n");
        return 1;
    }
    for (i = 1; i < argc; i++) {
        sh_target_t t;
        struct stat st;
        mode_t mode;
        char perms[11], when[40];

        if (resolve_arg(s, "stat", argv[i], &t) != 0) {
            status = 1;
            continue;
        }
        if (t.kind == SH_TARGET_ROOT) {
            sh_printf(s, "  File: %s\n  Type: synthetic root (lists devices)\n",
                      t.posix);
            continue;
        }
        if (stat_target(&t, &st) != 0) {
            sh_eprintf(s, "stat: %s: %s\n", argv[i], strerror(errno));
            status = 1;
            continue;
        }
        mode = (t.kind == SH_TARGET_PSEUDO || t.kind == SH_TARGET_PSEUDO_DIR)
                   ? st.st_mode
                   : synth_mode(&st, t.dev);
        format_time(when, sizeof(when), st.st_mtime, "%Y-%m-%d %H:%M:%S");
        sh_printf(s, "  File: %s\n", t.posix);
        sh_printf(s, "  Size: %-14lld  Type: %s\n", (long long)st.st_size,
                  S_ISDIR(st.st_mode) ? "directory" : "regular file");
        /* uid and gid are always 0 and always displayed as vita: the Vita has
         * no account database, so this is a display convention, not a claim.
         * The access line shows the synthesized mode, not the zero bits stat
         * hands back -- same reason as in ls -l. */
        sh_printf(s, "Access: (%04o/%s)  Uid: (0/vita)  Gid: (0/vita)\n",
                  (unsigned)(mode & 07777), mode_string(mode, perms));
        sh_printf(s, "Modify: %s\n", when);
    }
    return status;
}

static void print_device_usage(sh_session_t *s, const sh_device_t *dev)
{
    SceIoDevInfo info;
    char free_h[16], total_h[16];

    memset(&info, 0, sizeof(info));
    if (sceIoDevctl(dev->sce, DEVCTL_GET_DEV_INFO, NULL, 0, &info,
                    sizeof(info)) < 0) {
        sh_printf(s, "%-8s  %10s  %10s  (no device info)\n", dev->posix, "-",
                  "-");
        return;
    }
    /* No ux0 slack here: VitaShell pads the free space it will *attempt* to
     * copy by 40 MB, which is a UI decision. df reports the filesystem. */
    human_size(free_h, sizeof(free_h), (long long)info.free_size);
    human_size(total_h, sizeof(total_h), (long long)info.max_size);
    sh_printf(s, "%-8s  %10s  %10s  %s\n", dev->posix, free_h, total_h,
              dev->writable ? "rw" : "ro");
}

static int cmd_df(sh_session_t *s, int argc, char **argv)
{
    const sh_device_t *devices;
    size_t count, i;
    int status = 0;

    sh_printf(s, "%-8s  %10s  %10s  %s\n", "Filesystem", "Free", "Total",
              "Mode");
    if (argc > 1) {
        for (i = 1; i < (size_t)argc; i++) {
            sh_target_t t;

            if (resolve_arg(s, "df", argv[i], &t) != 0) {
                status = 1;
                continue;
            }
            if (t.dev == NULL) {
                sh_eprintf(s, "df: %s: Not a device\n", argv[i]);
                status = 1;
                continue;
            }
            print_device_usage(s, t.dev);
        }
        return status;
    }
    devices = sh_devices(&count);
    for (i = 0; i < count; i++) {
        if (sh_device_present(&devices[i])) {
            print_device_usage(s, &devices[i]);
        }
    }
    return 0;
}

static long long du_tree(sh_session_t *s, const sh_target_t *t, int depth,
                         sh_walk_t *w)
{
    DIR *dir;
    struct dirent *ent;
    long long total = 0;

    dir = opendir(t->sce);
    if (dir == NULL) {
        return 0;
    }
    while ((ent = readdir(dir)) != NULL) {
        struct stat st;

        sh_session_yield(s);
        if (sh_session_interrupted(s) || depth > MAX_DEPTH) {
            break;
        }
        if (sh_is_dot(ent->d_name)) {
            continue;
        }
        if (sh_join(w->a, sizeof(w->a), t->sce, ent->d_name) != 0) {
            continue;
        }
        if (stat(w->a, &st) != 0) {
            continue;
        }
        if (!S_ISDIR(st.st_mode)) {
            total += (long long)st.st_size;
            continue;
        }
        {
            sh_fsctx_t fs;
            sh_target_t child;

            fs.cwd = t->posix;
            fs.home = t->posix;
            if (sh_path_resolve(&fs, ent->d_name, &child) == 0) {
                total += du_tree(s, &child, depth + 1, w);
            }
        }
    }
    closedir(dir);
    return total;
}

static int cmd_du(sh_session_t *s, int argc, char **argv)
{
    const char *targets[MAX_ARGS];
    int i, n, status = 0;
    sh_walk_t *w;
    char human[16];

    n = collect_targets(argv, argc, "ah", targets, MAX_ARGS, NULL);
    if (n == 0) {
        targets[n++] = NULL; /* du with no argument means the cwd */
    }
    w = malloc(sizeof(*w));
    if (w == NULL) {
        sh_eprintf(s, "du: out of memory\n");
        return 1;
    }
    for (i = 0; i < n; i++) {
        sh_target_t t;

        if (resolve_arg(s, "du", targets[i], &t) != 0) {
            status = 1;
            continue;
        }
        if (t.kind == SH_TARGET_ROOT) {
            sh_eprintf(s, "du: %s: Invalid argument\n",
                       targets[i] ? targets[i] : ".");
            status = 1;
            continue;
        }
        human_size(human, sizeof(human), du_tree(s, &t, 0, w));
        sh_printf(s, "%8s  %s\n", human, t.posix);
    }
    free(w);
    return status;
}

/* ------------------------------------------------------------------ */
/* system information                                                   */
/* ------------------------------------------------------------------ */

/* env and printenv read the session's table (sshd_session.c); PWD is the one
 * variable computed here, from the cwd, so it cannot disagree with `cd`. */
static const char *env_lookup(sh_session_t *s, const char *name, const sh_fsctx_t *fs)
{
    if (strcmp(name, "PWD") == 0) {
        return fs->cwd;
    }
    return sh_env_get(sh_session_env(s), name);
}

static int cmd_env(sh_session_t *s, int argc, char **argv)
{
    sh_env_table_t *t = sh_session_env(s);
    sh_fsctx_t fs;
    int i;

    (void)argc;
    (void)argv;
    fs_of(s, &fs);
    for (i = 0; i < sh_env_count(t); i++) {
        sh_printf(s, "%s=%s\n", sh_env_name_at(t, i), sh_env_value_at(t, i));
    }
    sh_printf(s, "PWD=%s\n", fs.cwd);
    return 0;
}

static int cmd_printenv(sh_session_t *s, int argc, char **argv)
{
    sh_fsctx_t fs;
    int i, status = 0;

    if (argc == 1) {
        return cmd_env(s, argc, argv);
    }
    fs_of(s, &fs);
    for (i = 1; i < argc; i++) {
        const char *v = env_lookup(s, argv[i], &fs);

        if (v == NULL) {
            status = 1;
        } else {
            sh_printf(s, "%s\n", v);
        }
    }
    return status;
}

/* Who a download's file-name check speaks for, and whose session it is. */
typedef struct {
    sh_session_t *s;
    const char *tool;
} fetch_ctx_t;

/* Where curl or wget may put a file: resolved through the path layer, on a writable device, in a directory that exists. */
static int fetch_resolve_out(void *ctx, const char *name, char *sce, size_t cap)
{
    fetch_ctx_t *f = ctx;
    sh_session_t *s = f->s;
    sh_target_t t, dir;
    sh_fsctx_t fs;
    char parent[SH_POSIX_PATH_MAX];
    struct stat st;

    if (resolve_arg(s, f->tool, name, &t) != 0 || check_writable(s, f->tool, name, &t) != 0) {
        return -1;
    }
    fs_of(s, &fs);
    if (sh_parent(parent, sizeof(parent), t.posix) != 0 || sh_path_resolve(&fs, parent, &dir) != 0 ||
        dir.kind == SH_TARGET_ROOT || stat(dir.sce, &st) != 0 || !S_ISDIR(st.st_mode)) {
        sh_eprintf(s, "%s: %s: No such file or directory\n", f->tool, name);
        return -1;
    }
    if (strlen(t.sce) >= cap) {
        sh_eprintf(s, "%s: %s: %s\n", f->tool, name, strerror(ENAMETOOLONG));
        return -1;
    }
    memcpy(sce, t.sce, strlen(t.sce) + 1);
    return 0;
}

static int curl_usage(sh_session_t *s)
{
    sh_eprintf(s, "usage: curl [-LJOIfsSk] [-o FILE] URL\n");
    return 2;
}

static int cmd_curl(sh_session_t *s, int argc, char **argv)
{
    sshd_curl_opts_t o;
    fetch_ctx_t fc = {s, "curl"};
    int i, j, flags_done = 0;

    memset(&o, 0, sizeof(o));
    o.tool = "curl";
    o.insecure_flag = "-k";
    o.out_flag = "-o";
    o.resolve_out = fetch_resolve_out;
    o.ctx = &fc;
    for (i = 1; i < argc; i++) {
        const char *a = argv[i];

        if (flags_done || a[0] != '-' || a[1] == '\0') {
            if (o.url != NULL) {
                sh_eprintf(s, "curl: only one URL is supported\n");
                return 2;
            }
            o.url = a;
            continue;
        }
        if (strcmp(a, "--") == 0) {
            flags_done = 1;
            continue;
        }
        if (strcmp(a, "--insecure") == 0) {
            o.insecure = 1;
            continue;
        }
        for (j = 1; a[j] != '\0'; j++) {
            switch (a[j]) {
            case 'L': o.follow = 1; break;
            case 'O': o.by_url_name = 1; break;
            case 'J': o.use_dispo = 1; break;
            case 'I': o.head_only = 1; break;
            case 'f': o.fail_http = 1; break;
            case 's': o.silent = 1; break;
            case 'S': o.show_errors = 1; break;
            case 'k': o.insecure = 1; break;
            case 'o':
                if (a[j + 1] != '\0') {
                    o.out_arg = a + j + 1;
                } else if (i + 1 < argc) {
                    o.out_arg = argv[++i];
                } else {
                    sh_eprintf(s, "curl: -o needs a file name\n");
                    return 2;
                }
                j = (int)strlen(a) - 1;
                break;
            default:
                sh_eprintf(s, "curl: -%c: unknown option\n", a[j]);
                return curl_usage(s);
            }
        }
    }
    if (o.url == NULL) {
        return curl_usage(s);
    }
    if (o.out_arg != NULL && o.by_url_name) {
        sh_eprintf(s, "curl: -o and -O cannot be combined\n");
        return 2;
    }
    if (o.use_dispo && !o.by_url_name) {
        sh_eprintf(s, "curl: -J needs -O\n");
        return 2;
    }
    if (o.head_only && (o.out_arg != NULL || o.by_url_name)) {
        sh_eprintf(s, "curl: -I prints to the terminal; redirect it with >\n");
        return 2;
    }
    return sshd_curl_run(s, &o);
}

/* wget is curl's engine with wget's defaults: follow redirects, fail on an HTTP error, save under the URL's name. */
static int cmd_wget(sh_session_t *s, int argc, char **argv)
{
    sshd_curl_opts_t o;
    sh_wget_args_t a;
    fetch_ctx_t fc = {s, "wget"};
    char msg[160];
    int rc;

    rc = sh_wget_parse(argc, argv, &a, msg, sizeof(msg));
    if (rc < 0) {
        sh_eprintf(s, "wget: %s\n", msg);
        return 2;
    }
    if (rc > 0) {
        sh_eprintf(s, "usage: wget [-q] [-O FILE] [--no-check-certificate] [--content-disposition] URL\n");
        return 2;
    }
    memset(&o, 0, sizeof(o));
    o.tool = "wget";
    o.insecure_flag = "--no-check-certificate";
    o.out_flag = "-O";
    o.url = a.url;
    o.out_arg = a.out_file;
    o.by_url_name = a.out_file == NULL && !a.to_terminal;
    o.use_dispo = a.use_dispo && o.by_url_name;
    o.follow = 1;
    o.fail_http = 1;
    o.silent = a.quiet;
    o.show_errors = a.quiet;
    o.insecure = a.insecure;
    o.resolve_out = fetch_resolve_out;
    o.ctx = &fc;
    return sshd_curl_run(s, &o);
}

static int cmd_vpk(sh_session_t *s, int argc, char **argv)
{
    sh_target_t t;
    int replace = 0, file = 2;
    int is_reinstall = argc > 2 && (strcmp(argv[2], "-r") == 0 || strcmp(argv[2], "--reinstall") == 0);

    if (argc == 4 && strcmp(argv[1], "install") == 0 && is_reinstall) {
        replace = 1;
        file = 3;
    } else if (argc != 3 || (strcmp(argv[1], "list") != 0 && strcmp(argv[1], "install") != 0) || is_reinstall) {
        sh_eprintf(s, "usage: vpk list FILE | vpk install [-r | --reinstall] FILE\n");
        return 2;
    }
    if (resolve_arg(s, "vpk", argv[file], &t) != 0) {
        return 1;
    }
    if (t.kind == SH_TARGET_ROOT || t.dev == NULL) {
        sh_eprintf(s, "vpk: %s: Not a file\n", argv[file]);
        return 1;
    }
    return (argv[1][0] == 'l') ? sh_vpk_list(s, t.sce, argv[file])
                               : sh_vpk_install(s, t.sce, argv[file], replace);
}

static int cmd_which(sh_session_t *s, int argc, char **argv)
{
    int i, status = 0;

    if (argc < 2) {
        sh_eprintf(s, "usage: which COMMAND...\n");
        return 1;
    }
    for (i = 1; i < argc; i++) {
        /* PATH is decorative (see the table seeded in sh_session_new): everything this shell can run
         * is in the builtin table, so "found" and "builtin" are the same
         * event. Saying so beats the traditional "in (/bin:/usr/bin)", which
         * would claim a search that would find nothing. */
        const char *alias = sh_alias_get(sh_session_aliases(s), argv[i]);

        if (alias != NULL) {
            sh_printf(s, "%s: aliased to %s\n", argv[i], alias);
        } else if (sh_builtin_lookup(argv[i]) != NULL) {
            sh_printf(s, "%s: shell builtin\n", argv[i]);
        } else {
            sh_eprintf(s, "which: no %s in the builtins\n", argv[i]);
            status = 1;
        }
    }
    return status;
}

/*
 * Prints the session's command history, oldest line numbered 1 -- the same
 * numbering `!n` in the editor expands against, so the two can never disagree
 * about what line 7 is. `history N` shows only the last N, with their real
 * numbers, like bash. History lives per connection and dies with it; there is
 * no ~/.bash_history equivalent yet.
 */
static int cmd_history(sh_session_t *s, int argc, char **argv)
{
    const char *entries[SSHD_HISTORY];
    int n, i, start = 0;

    if (argc > 2) {
        sh_eprintf(s, "usage: history [COUNT]\n");
        return 1;
    }
    n = sh_session_history(s, entries, SSHD_HISTORY);
    if (argc == 2) {
        char *end;
        long want = strtol(argv[1], &end, 10);

        if (end == argv[1] || *end != '\0' || want < 0) {
            sh_eprintf(s, "history: %s: not a number\n", argv[1]);
            return 1;
        }
        /* The last WANT entries, numbered by their real position, so the
         * numbers match what `!n` expands against. */
        start = n - (int)want;
        if (start < 0) {
            start = 0;
        }
    }
    for (i = start; i < n; i++) {
        sh_printf(s, "%4d  %s\n", i + 1, entries[i]);
    }
    return 0;
}

static int cmd_date(sh_session_t *s, int argc, char **argv)
{
    char when[64];
    time_t now = time(NULL);

    (void)argc;
    (void)argv;
    format_time(when, sizeof(when), now, "%a %b %e %H:%M:%S %Y");
    sh_printf(s, "%s\n", when);
    return 0;
}

/* uname(1); the release comes from the kernel, not from a literal. */
static int cmd_uname(sh_session_t *s, int argc, char **argv)
{
    sh_sysinfo_t info;

    /* Bare uname prints only the system name: no kernel call, no new text. */
    if (argc < 2 || strchr(argv[1], 'a') == NULL) {
        sh_printf(s, "Vita\n");
        return 0;
    }
    /* Name, kernel-reported release and machine, so nothing needs keeping current. */
    sh_sysinfo_get(&info);
    sh_printf(s, "Vita vita %s VitaSDK %s\n", info.firmware,
              info.vitatv ? "armv7a (vitatv)" : "armv7a");
    return 0;
}

static int cmd_whoami(sh_session_t *s, int argc, char **argv)
{
    (void)argc;
    (void)argv;
    sh_printf(s, "%s\n", sh_session_user(s));
    return 0;
}

static int cmd_devices(sh_session_t *s, int argc, char **argv)
{
    const sh_device_t *devices;
    size_t count, i;

    (void)argc;
    (void)argv;
    devices = sh_devices(&count);
    for (i = 0; i < count; i++) {
        sh_printf(s, "%-8s %-8s %s%s\n", devices[i].posix, devices[i].sce,
                  sh_device_present(&devices[i]) ? "present" : "not mounted",
                  devices[i].writable ? "" : "  (read-only)");
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* help, exit                                                           */
/* ------------------------------------------------------------------ */

/* alias NAME='VALUE' is the form sh prints, so a listing can be pasted back in. */
static void print_alias(sh_session_t *s, const char *name, const char *value)
{
    const char *q;

    sh_printf(s, "alias %s='", name);
    while ((q = strchr(value, '\'')) != NULL) {
        sh_write(s, value, (size_t)(q - value));
        sh_printf(s, "'\\''");
        value = q + 1;
    }
    sh_printf(s, "%s'\n", value);
}

static int cmd_alias(sh_session_t *s, int argc, char **argv)
{
    sh_alias_table_t *t = sh_session_aliases(s);
    int i, status = 0;

    if (t == NULL) {
        sh_eprintf(s, "alias: aliases are unavailable in this session\n");
        return 1;
    }
    if (argc == 1) {
        for (i = 0; i < sh_alias_count(t); i++) {
            print_alias(s, sh_alias_name_at(t, i), sh_alias_value_at(t, i));
        }
        return 0;
    }
    for (i = 1; i < argc; i++) {
        char *eq = strchr(argv[i], '=');

        if (eq == NULL) {
            const char *value = sh_alias_get(t, argv[i]);

            if (value == NULL) {
                sh_eprintf(s, "alias: %s: not found\n", argv[i]);
                status = 1;
            } else {
                print_alias(s, argv[i], value);
            }
            continue;
        }
        *eq = '\0';
        if (sh_alias_set(t, argv[i], eq + 1) != 0) {
            switch (errno) {
            case ENOSPC:
                sh_eprintf(s, "alias: too many aliases (limit %d)\n", SH_ALIAS_MAX);
                break;
            case ELOOP:
                sh_eprintf(s, "alias: %s: that value would make a loop of aliases\n", argv[i]);
                break;
            case ENAMETOOLONG:
                sh_eprintf(s, "alias: %s: name over %d or value over %d bytes\n", argv[i], SH_ALIAS_NAME_MAX,
                           SH_ALIAS_VALUE_MAX);
                break;
            default:
                sh_eprintf(s, "alias: %s: %s\n", argv[i],
                           sh_alias_name_ok(argv[i]) ? "the value is empty" : "not a valid alias name");
                break;
            }
            status = 1;
        }
    }
    return status;
}

static int cmd_unalias(sh_session_t *s, int argc, char **argv)
{
    sh_alias_table_t *t = sh_session_aliases(s);
    int i, status = 0;

    if (argc < 2) {
        sh_eprintf(s, "usage: unalias NAME...\n");
        return 1;
    }
    for (i = 1; i < argc; i++) {
        if (sh_alias_unset(t, argv[i]) != 0) {
            sh_eprintf(s, "unalias: %s: not found\n", argv[i]);
            status = 1;
        }
    }
    return status;
}

static int cmd_help(sh_session_t *s, int argc, char **argv)
{
    const sh_builtin_t *table;
    size_t count, i;

    table = sh_builtins(&count);
    if (argc > 1) {
        const char *alias = sh_alias_get(sh_session_aliases(s), argv[1]);

        if (alias != NULL) {
            sh_printf(s, "%s is an alias for `%s`%s\n", argv[1], alias,
                      sh_builtin_lookup(argv[1]) != NULL ? "; it wins over the builtin of the same name" : "");
            return 0;
        }
        for (i = 0; i < count; i++) {
            if (strcmp(table[i].name, argv[1]) == 0) {
                sh_printf(s, "%s\n", table[i].usage);
                return 0;
            }
        }
        sh_eprintf(s, "help: no such command: %s\n", argv[1]);
        return 1;
    }
    sh_printf(s, "Builtins:");
    for (i = 0; i < count; i++) {
        sh_printf(s, " %s", table[i].name);
    }
    sh_printf(s, "\nType `help COMMAND` for one line of usage. Paths start at"
                 " `/`: `/` lists the devices, `/ux0` is the memory card. Join up to %d commands"
                 " with ; && || (each a separate word, as with | and >). `alias` lists the shortcuts; an alias"
                 " expands only as the command word, follows other aliases (each at most once), and wins over a"
                 " builtin of the same name.\n",
              SH_CHAIN_MAX);
    return 0;
}

static int cmd_exit(sh_session_t *s, int argc, char **argv)
{
    (void)argc;
    (void)argv;
    sh_session_request_exit(s);
    return 0;
}

/* true, false and `:` ignore their arguments, as POSIX specifies; `:` shares cmd_true. */
static int cmd_true(sh_session_t *s, int argc, char **argv)
{
    (void)s;
    (void)argc;
    (void)argv;
    return 0;
}

static int cmd_false(sh_session_t *s, int argc, char **argv)
{
    (void)s;
    (void)argc;
    (void)argv;
    return 1;
}

/* ------------------------------------------------------------------ */
/* vcmd: one line to a TCP port, the reply to stdout                  */
/* ------------------------------------------------------------------ */

/* Whole-exchange budget. A builtin runs inside the server's single poll loop,
 * so this is not politeness: every other session, the console and the pad are
 * frozen while it counts down. Five seconds covers a LAN round-trip and keeps a
 * dead peer to a hitch rather than the eleven-minute stall a bare blocking
 * connect() to an unanswered address has already cost this server once. */
#define NET_DEADLINE_S  5
#define NET_POLL_MS     50
#define NET_LINE_MAX    1024       /* what we send                    */
#define NET_REPLY_MAX   (64 * 1024) /* what we are willing to read back */

/* Waits for fd to satisfy events, running the server's poll loop meanwhile so
 * every other session's socket keeps being read. This is why the command is
 * written this way and not as a plain connect/send/recv: a builtin may never sit
 * in a blocking socket call, because there is no thread to spare and no Ctrl-C
 * to notice -- seeing the keystroke would need the very socket being blocked on.
 * So each step is non-blocking and re-checked here, in the same discipline
 * wait_for_room() uses for the client's window. 0 when ready, -1 on timeout or
 * error with errno set. */
static int net_wait(sh_session_t *s, int fd, short events, time_t deadline)
{
    struct pollfd pfd;

    pfd.fd = fd;
    pfd.events = events;
    pfd.revents = 0;
    for (;;) {
        int ready = poll(&pfd, 1, 0);

        if (ready > 0) {
            if ((pfd.revents & (POLLERR | POLLNVAL)) != 0) {
                errno = EIO;
                return -1;
            }
            return 0; /* POLLHUP falls through: the read below sees the EOF */
        }
        if (ready < 0 && errno != EINTR) {
            return -1;
        }
        if (sh_session_interrupted(s)) {
            errno = EINTR;
            return -1;
        }
        if (time(NULL) >= deadline) {
            errno = ETIMEDOUT;
            return -1;
        }
        sh_server_poll_slice(NET_POLL_MS);
    }
}

/* Sends one newline-terminated request and prints what comes back. Named for the
 * plugin's own tools/bench/vcmd.py client, which this replaces for anyone already
 * sitting in an ssh session; the command itself is generic, not Wi-Fi Turbo's --
 * any newline-request/response port will do, the debug socket on :1339 or the
 * vitacompanion command port on :1338. It is deliberately not called netcat: that
 * name promises a bidirectional pipe this is not, and the real nc on a host can
 * do things a builtin in a single-threaded poll loop must not attempt.
 *
 * Loopback matters here and it is asymmetric: 127.0.0.1:1339 answers fine,
 * because that listener lives on the plugin's own thread and keeps running while
 * this command waits. 127.0.0.1:22 cannot work -- that is this server, whose
 * poll loop is the one being held -- so it just spends the deadline and gives up.
 * Bounded, but worth knowing before reaching for it as a self-test. */
static int cmd_vcmd(sh_session_t *s, int argc, char **argv)
{
    struct sockaddr_in sa;
    char *buf;
    char *out;
    size_t used = 0;
    time_t deadline;
    int fd, port, i, off, status = 0;
    int why, soerr;

    if (argc < 3) {
        sh_eprintf(s, "vcmd: usage: vcmd HOST PORT [LINE]\n");
        return 1;
    }
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    if (inet_pton(AF_INET, argv[1], &sa.sin_addr) != 1) {
        /* Names are refused rather than resolved: gethostbyname() would block
         * this thread for however long the resolver takes, with no bound to put
         * on it, which is the one thing a builtin may not do. Ask for the
         * address and say so plainly instead of half-supporting hostnames. */
        sh_eprintf(s, "vcmd: %s: not an IPv4 address (names are not resolved)\n",
                   argv[1]);
        return 1;
    }
    errno = 0;
    port = (int)strtol(argv[2], NULL, 10);
    if (errno != 0 || port <= 0 || port > 65535) {
        sh_eprintf(s, "vcmd: %s: bad port\n", argv[2]);
        return 1;
    }
    sa.sin_port = htons((unsigned short)port);

    out = malloc(NET_LINE_MAX);
    buf = malloc(NET_REPLY_MAX);
    if (out == NULL || buf == NULL) {
        free(out);
        free(buf);
        sh_eprintf(s, "vcmd: out of memory\n");
        return 1;
    }

    /* The request: remaining arguments space-separated, one line. With none, a
     * bare newline, which is enough to poke a server that needs no argument. */
    out[0] = '\0';
    for (i = 3; i < argc; i++) {
        int put = snprintf(out + used, NET_LINE_MAX - used, "%s%s",
                           used == 0 ? "" : " ", argv[i]);

        if (put < 0 || (size_t)put >= NET_LINE_MAX - used) {
            free(out);
            free(buf);
            sh_eprintf(s, "vcmd: request too long\n");
            return 1;
        }
        used += (size_t)put;
    }
    if (used + 1 >= NET_LINE_MAX) {
        free(out);
        free(buf);
        sh_eprintf(s, "vcmd: request too long\n");
        return 1;
    }
    out[used++] = '\n';

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        sh_eprintf(s, "vcmd: socket: %s\n", strerror(errno));
        free(out);
        free(buf);
        return 1;
    }
    if (fcntl(fd, F_SETFL, O_NONBLOCK) != 0) {
        sh_eprintf(s, "vcmd: %s: %s\n", argv[1], strerror(errno));
        close(fd);
        free(out);
        free(buf);
        return 1;
    }
    deadline = time(NULL) + NET_DEADLINE_S;

    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0 && errno != EINPROGRESS) {
        sh_eprintf(s, "vcmd: %s:%d: %s\n", argv[1], port, strerror(errno));
        close(fd);
        free(out);
        free(buf);
        return 1;
    }
    /* A refused connection completes as writability, with the cause visible only
     * through SO_ERROR, so even a successful wait has to be interrogated. Skip
     * that and a closed port on loopback surfaces as EPIPE from the send below
     * and reads as "Broken pipe" -- the one misdiagnosis worth avoiding here,
     * because "refused" and "timed out" are different answers and telling them
     * apart is the whole reason this command exists. */
    why = 0;
    soerr = 0;
    if (net_wait(s, fd, POLLOUT, deadline) != 0) {
        why = errno;
    }
    {
        int err = 0;
        socklen_t len = sizeof(err);

        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) == 0 && err != 0) {
            why = err; /* beats a bare ETIMEDOUT: it names the actual cause */
            soerr = err;
        }
    }
    if (why != 0) {
        /* sceNet hands back 0x80...... codes newlib's strerror() cannot name, so
         * the text alone can print "No data" for what is really a refused port.
         * Print the number beside it: refused-versus-timed-out is the distinction
         * that matters here and it must survive the wrong label. */
        if (soerr != 0) {
            sh_eprintf(s, "vcmd: %s:%d: connect failed: %s (socket error 0x%08x)\n",
                       argv[1], port, strerror(why), (unsigned)soerr);
        } else {
            sh_eprintf(s, "vcmd: %s:%d: %s\n", argv[1], port, strerror(why));
        }
        close(fd);
        free(out);
        free(buf);
        return 1;
    }

    off = 0;
    while (off < (int)used && status == 0) {
        ssize_t put;

        if (net_wait(s, fd, POLLOUT, deadline) != 0) {
            sh_eprintf(s, "vcmd: %s:%d: %s while sending\n", argv[1], port,
                       strerror(errno));
            status = 1;
            break;
        }
        put = send(fd, out + off, used - (size_t)off, 0);
        if (put == 0) {
            /* Nothing accepted and no error: the peer is gone. Treating it as
             * progress would spin here at full CPU with the poll loop held, so
             * stop rather than spend the deadline burning the device. */
            sh_eprintf(s, "vcmd: %s:%d: connection closed while sending\n",
                       argv[1], port);
            status = 1;
            break;
        }
        if (put < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                sh_server_poll_slice(NET_POLL_MS);
                continue;
            }
            sh_eprintf(s, "vcmd: %s:%d: %s while sending\n", argv[1], port,
                       strerror(errno));
            status = 1;
            break;
        }
        off += (int)put;
    }

    if (status == 0) {
        size_t total = 0;

        for (;;) {
            ssize_t got;

            if (net_wait(s, fd, POLLIN, deadline) != 0) {
                /* A peer that answers and holds the socket open never sends EOF,
                 * so the deadline is what ends the read. Got bytes already means
                 * the reply is good; a timeout with nothing at all is a failure. */
                if (errno != ETIMEDOUT || total == 0) {
                    sh_eprintf(s, "vcmd: %s:%d: %s while reading\n", argv[1], port,
                               strerror(errno));
                    status = 1;
                }
                break;
            }
            got = recv(fd, buf, NET_REPLY_MAX - total, 0);
            if (got < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                    sh_server_poll_slice(NET_POLL_MS);
                    continue;
                }
                sh_eprintf(s, "vcmd: %s:%d: %s while reading\n", argv[1], port,
                           strerror(errno));
                status = 1;
                break;
            }
            if (got == 0) {
                break; /* peer half-closed: the reply is complete */
            }
            /* sh_write, not sh_printf: a reply is arbitrary bytes and must never
             * be read as a format string, and this is the one output call that
             * also honors `vcmd ... > /ux0/data/x`. */
            sh_write(s, buf, (size_t)got);
            total += (size_t)got;
            if (total >= NET_REPLY_MAX) {
                sh_eprintf(s, "vcmd: reply truncated at %u bytes\n",
                           (unsigned)total);
                break;
            }
        }
    }

    close(fd);
    free(out);
    free(buf);
    return status;
}

/* ------------------------------------------------------------------ */
/* the table                                                            */
/* ------------------------------------------------------------------ */

/* Keep this alphabetical: `help` prints it in order and people scan it. */
static const sh_builtin_t builtins[] = {
    {":", cmd_true, ": [ARG...] -- do nothing, successfully"},
    {"alias", cmd_alias,
     "alias [NAME[=VALUE]]... -- list aliases, show one, or define one; expands as the command word"},
    {"cat", cmd_cat, "cat FILE... -- write a file to the terminal"},
    {"cd", cmd_cd, "cd [DIR] -- change the working directory (no argument: $HOME)"},
    {"cp", cmd_cp, "cp [-r] SRC... DST -- copy; -r for directories"},
    {"curl", cmd_curl,
     "curl [-LJOIfsSk] [-o FILE] URL -- fetch an http(s) URL; -L redirects, -O/-J name the file, -I headers, -f fail on HTTP errors, -s/-S quiet, -k skip certificate checks"},
    {"date", cmd_date, "date -- print the current date and time"},
    {"devices", cmd_devices, "devices -- every Vita device and whether it is mounted"},
    {"df", cmd_df, "df [DEVICE] -- free and total space per device"},
    {"du", cmd_du, "du [PATH] -- total size of a tree"},
    {"echo", cmd_echo, "echo [-n] TEXT... -- print its arguments"},
    {"env", cmd_env, "env -- print the environment"},
    {"exit", cmd_exit, "exit -- end this session"},
    {"false", cmd_false, "false [ARG...] -- do nothing, unsuccessfully (status 1)"},
    {"grep", cmd_grep,
     "grep [-ivnclq] PATTERN [FILE...] -- lines matching a POSIX basic regex"},
    {"head", cmd_head,
     "head [-n N | -c N] [FILE] -- first N lines (10) or N bytes of a file or stdin"},
    {"help", cmd_help, "help [COMMAND] -- usage"},
    {"history", cmd_history, "history [COUNT] -- show numbered command history (!n re-runs line n)"},
    {"ls", cmd_ls, "ls [-l] [-a] [PATH] -- list a directory (no argument: .)"},
    {"mkdir", cmd_mkdir, "mkdir [-p] DIR... -- create directories"},
    {"mv", cmd_mv, "mv SRC... DST -- rename, copying across devices"},
    {"printenv", cmd_printenv, "printenv [NAME...] -- print variable values (all of env with no NAME; status 1 if one is unset)"},
    {"pwd", cmd_pwd, "pwd -- print the working directory"},
    {"rm", cmd_rm, "rm [-r] [-f] PATH... -- delete; -r for directories"},
    {"rmdir", cmd_rmdir, "rmdir DIR... -- remove an empty directory"},
    {"sort", cmd_sort,
     "sort [-n] [-r] [-u] [FILE] -- order lines, -n numeric, -r reverse, -u unique keys"},
    {"stat", cmd_stat, "stat PATH... -- size, mode and modification time"},
    {"tail", cmd_tail,
     "tail [-n N | -c N] [FILE] -- last N lines (10) or N bytes of a file or stdin"},
    {"touch", cmd_touch, "touch FILE... -- create a file or update its time"},
    {"true", cmd_true, "true [ARG...] -- do nothing, successfully (status 0)"},
    {"unalias", cmd_unalias, "unalias NAME... -- remove aliases"},
    {"uname", cmd_uname,
     "uname [-a] -- system name; -a adds the firmware the device reports"},
    {"uniq", cmd_uniq,
     "uniq [-c] [-d] [-u] [FILE] -- collapse adjacent repeats; -c counts, -d repeats only, -u once-only"},
    {"vcmd", cmd_vcmd, "vcmd HOST PORT [LINE] -- send one line to a TCP port and print the reply"},
    {"vpk", cmd_vpk,
     "vpk list FILE | vpk install [-r | --reinstall] FILE -- inspect a VPK, or install it: this app updates itself, another title installs unless it is running, --reinstall (-r) replaces an installed one"},
    {"wc", cmd_wc, "wc [-lwc] [FILE...] -- count lines, words and bytes of files or stdin"},
    {"wget", cmd_wget,
     "wget [-q] [-O FILE] [--no-check-certificate] [--content-disposition] URL -- fetch an http(s) URL into the current directory under its name; same engine as curl -L -f -O"},
    {"which", cmd_which, "which COMMAND... -- report whether COMMAND is one of these builtins"},
    {"whoami", cmd_whoami, "whoami -- the name you logged in as"},
};

const sh_builtin_t *sh_builtins(size_t *count)
{
    if (count != NULL) {
        *count = sizeof(builtins) / sizeof(builtins[0]);
    }
    return builtins;
}

const sh_builtin_t *sh_builtin_lookup(const char *name)
{
    size_t count, i;
    const sh_builtin_t *table = sh_builtins(&count);

    for (i = 0; i < count; i++) {
        if (strcmp(table[i].name, name) == 0) {
            return &table[i];
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* dispatch                                                             */
/* ------------------------------------------------------------------ */

#define MAX_WORDS (MAX_ARGS * 2)

/* Closes the operator word at i and the file name after it, which the caller has
 * already taken. Shifting once instead left the filename alive as a plain argument,
 * and `echo hello > f.txt` printed "hello f.txt" into f.txt. */
static void drop_operator(int *argc, char **argv, int *quoted, int i)
{
    int j, k;

    free(argv[i]);
    for (k = 0; k < 2; k++) {
        for (j = i; j < *argc - 1; j++) {
            argv[j] = argv[j + 1];
            quoted[j] = quoted[j + 1];
        }
        (*argc)--;
    }
}

/* Pulls the `<` word and the file name after it, like extract_redirect: both must be
 * separate, unquoted words, and a second `<` replaces the first. The caller frees
 * *path. Returns 0, or -1 after reporting a dangling operator. */
static int extract_stdin(sh_session_t *s, int *argc, char **argv, int *quoted,
                         char **path)
{
    int i;

    for (i = 1; i < *argc; i++) {
        if (quoted[i] || strcmp(argv[i], "<") != 0) {
            continue;
        }
        if (i + 1 >= *argc) {
            sh_eprintf(s, "sh: syntax error: file name expected after <\n");
            return -1;
        }
        free(*path);
        *path = argv[i + 1];
        drop_operator(argc, argv, quoted, i);
        i--;
    }
    return 0;
}

static ssize_t target_fd_reader(void *ctx, void *buf, size_t n)
{
    return sh_target_read(*(int *)ctx, buf, n);
}

/* Opens the `<` target and loads it as the session's stdin, reporting any failure
 * in the shell's own voice. Returns 0, or -1 when no command should run. */
static int start_stdin(sh_session_t *s, const char *arg)
{
    sh_target_t t;
    struct stat st;
    int fd, rc, saved;

    if (resolve_arg(s, "sh", arg, &t) != 0) {
        return -1;
    }
    if (stat_target(&t, &st) == 0 && S_ISDIR(st.st_mode)) {
        sh_eprintf(s, "sh: %s: Is a directory\n", arg);
        return -1;
    }
    fd = sh_target_open(&t);
    if (fd == -1) {
        sh_eprintf(s, "sh: %s: %s\n", arg, strerror(errno));
        return -1;
    }
    rc = sh_session_stdin_start(s, target_fd_reader, &fd);
    saved = errno;
    sh_target_close(fd);
    if (rc != 0) {
        if (saved == EFBIG) {
            sh_eprintf(s, "sh: %s: too large for stdin (limit %d bytes)\n", arg,
                       SSHD_STDIN_MAX);
        } else {
            sh_eprintf(s, "sh: %s: %s\n", arg, strerror(saved));
        }
        return -1;
    }
    return 0;
}

/*
 * Pulls redirection words off the argument list: the `>` or `>>` word and the
 * file name after it. Both must be separate words (`echo hi > /f/x`), because
 * split_line() has no notion of operator characters; the glued form `hi>/f/x`
 * arrives as one word and is deliberately left alone rather than guessed at.
 * A second redirect replaces the first, like sh, so `cmd > a > b` writes b.
 * Returns 0 with the target word and append flag set (or a NULL target when
 * there was none), or -1 after reporting a dangling operator.
 */
static int extract_redirect(sh_session_t *s, int *argc, char **argv,
                            int *quoted, char **path, int *append)
{
    int i;

    for (i = 1; i < *argc; i++) {
        if (quoted[i] || (strcmp(argv[i], ">") != 0 &&
                          strcmp(argv[i], ">>") != 0)) {
            continue;
        }
        if (i + 1 >= *argc) {
            sh_eprintf(s, "sh: syntax error: file name expected after %s\n",
                       argv[i]);
            return -1;
        }
        *append = (argv[i][1] == '>');
        /* The caller gets the filename and frees it. */
        *path = argv[i + 1];
        drop_operator(argc, argv, quoted, i);
        i--; /* the shifted-down tail may hold another redirect */
    }
    return 0;
}

/* Replaces argv[0] with the words of one alias value, in place. Returns 0, or -1 after printing when the
 * words would not fit. */
static int splice_alias_value(sh_session_t *s, char **argv, int *quoted, int *argc, const char *value)
{
    char *words[MAX_WORDS];
    int wordq[MAX_WORDS];
    int n;

    n = split_line(value, words, wordq, MAX_WORDS);
    if (n + *argc - 1 > MAX_WORDS) {
        free_args(words, n);
        sh_eprintf(s, "sh: %s: alias expands to too many words\n", argv[0]);
        return -1;
    }
    free(argv[0]);
    memmove(argv + n, argv + 1, (size_t)(*argc - 1) * sizeof(*argv));
    memmove(quoted + n, quoted + 1, (size_t)(*argc - 1) * sizeof(*quoted));
    memcpy(argv, words, (size_t)n * sizeof(*argv));
    memcpy(quoted, wordq, (size_t)n * sizeof(*quoted));
    *argc += n - 1;
    return 0;
}

/* Expands an unquoted command word that names an alias, as bash does: the alias, then the first word of
 * its value if that is an alias, and so on, each alias at most once (sh_alias_chain decides the order and
 * the stop). This runs before redirects are extracted, so an alias followed by `> file` redirects and a
 * `>` inside a value acts like the text it stands for. A chain or pipe operator in a value is an ordinary
 * argument, because the line was split before this point. Returns 0, or -1 after printing. */
static int expand_alias(sh_session_t *s, char **argv, int *quoted, int *argc)
{
    const char *chain[SH_ALIAS_MAX];
    int i, steps;

    if (quoted[0]) {
        return 0;
    }
    steps = sh_alias_chain(sh_session_aliases(s), argv[0], chain, SH_ALIAS_MAX);
    for (i = 0; i < steps; i++) {
        if (splice_alias_value(s, argv, quoted, argc, chain[i]) != 0) {
            return -1;
        }
    }
    return 0;
}

/* Runs one command from its already split words, which it takes ownership of and
 * frees on every path. argv and quoted hold MAX_WORDS entries: glob expansion grows
 * the list in place. */
static int run_command(sh_session_t *s, char **argv, int *quoted, int argc)
{
    int i, status = 0;
    const sh_builtin_t *cmd;
    char *redir_path = NULL; /* owns the filename word extract_redirect took
                              * out of argv; freed on every path below */
    int redir_append = 0;
    char *in_path = NULL; /* the `<` filename, owned the same way */
    int in_started = 0;

    if (argc == 0) {
        return 0;
    }
    if (expand_alias(s, argv, quoted, &argc) != 0) {
        free_args(argv, argc);
        return 1;
    }
    if (argc == 0) {
        return 0;
    }
    /* Redirection words leave the list FIRST, so the glob loop never sees
     * them: a redirect target is a file to create, not a pattern to expand. */
    if (extract_redirect(s, &argc, argv, quoted, &redir_path,
                         &redir_append) != 0 ||
        extract_stdin(s, &argc, argv, quoted, &in_path) != 0) {
        free(redir_path);
        free(in_path);
        free_args(argv, argc);
        return 1;
    }

    /* Expand wildcards unless the word was quoted, which is how a quoted "*"
     * stays literal. The command name itself is never a pattern. */
    for (i = 1; i < argc; i++) {
        char *expanded[MAX_LIST];
        sh_fsctx_t fs;
        int n, room;

        if (quoted[i] || !has_glob_chars(argv[i])) {
            continue;
        }
        fs_of(s, &fs);
        n = expand_glob(&fs, argv[i], expanded, MAX_LIST);
        if (n == 0) {
            continue; /* keep the word as typed, as sh does */
        }
        room = MAX_WORDS - (argc - 1);
        if (n > room) {
            int j;

            for (j = n - 1; j >= room; j--) {
                free(expanded[j]);
            }
            n = room;
        }
        free(argv[i]);
        {
            int j;

            for (j = argc - 1; j > i; j--) {
                argv[j + n - 1] = argv[j];
            }
            for (j = 0; j < n; j++) {
                argv[i + j] = expanded[j];
                quoted[i + j] = 1; /* expanded names must not glob again */
            }
            argc += n - 1;
        }
    }

    /* `command < file`: loaded before anything is created, so a missing input runs
     * no command and leaves no empty output file behind. */
    if (in_path != NULL) {
        if (start_stdin(s, in_path) != 0) {
            status = 1;
            goto out;
        }
        in_started = 1;
    }

    /* `command > file` / `>>`: the target is resolved like any path argument,
     * opened before the builtin runs, closed after, on every path below. sh
     * semantics kept where they cost nothing: the file is created even when
     * the command is not found, and stderr (sh_eprintf) stays on the
     * terminal. The write bit is checked here rather than left to fopen so a
     * read-only device reports "read-only file system", the same message
     * mkdir gives, instead of an opaque sceIo failure. */
    if (redir_path != NULL) {
        sh_target_t t;

        if (resolve_arg(s, "sh", redir_path, &t) != 0) {
            /* resolve_arg already reported it in the command's own voice */
            status = 1;
            goto out;
        }
        if (t.kind == SH_TARGET_ROOT || t.kind == SH_TARGET_DEVICE ||
            t.kind == SH_TARGET_PSEUDO || t.kind == SH_TARGET_PSEUDO_DIR) {
            /* A synthetic name has no file behind it, so nothing can be written. */
            sh_eprintf(s, "sh: %s: Permission denied\n", redir_path);
            status = 1;
            goto out;
        }
        if (t.dev != NULL && !t.dev->writable) {
            sh_eprintf(s, "sh: %s: read-only file system\n", redir_path);
            status = 1;
            goto out;
        }
        if (sh_session_redirect_start(s, t.sce, redir_append) != 0) {
            sh_eprintf(s, "sh: %s: %s\n", redir_path, strerror(errno));
            status = 1;
            goto out;
        }
    }

    cmd = sh_builtin_lookup(argv[0]);
    if (cmd == NULL) {
        sh_eprintf(s, "sh: %s: command not found\n", argv[0]);
        status = 127;
    } else {
        status = cmd->fn(s, argc, argv);
    }
    if (redir_path != NULL) {
        sh_session_redirect_end(s);
    }
out:
    if (in_started) {
        sh_session_stdin_end(s);
    }
    free(in_path);
    free(redir_path);
    free_args(argv, argc);
    return status;
}

static int is_pipe_word(const char *word, int quoted)
{
    return !quoted && strcmp(word, "|") == 0;
}

/* The `|` words must be separate and unquoted, like `>` and `<`: a glued `a|b` stays one
 * word and a quoted "|" is an argument. Returns 1 when a segment would be empty. */
static int has_empty_segment(char **argv, int *quoted, int argc)
{
    int i;

    for (i = 0; i < argc; i++) {
        if (is_pipe_word(argv[i], quoted[i]) &&
            (i == 0 || i == argc - 1 || is_pipe_word(argv[i + 1], quoted[i + 1]))) {
            return 1;
        }
    }
    return 0;
}

/* Runs the segments left to right. Each one but the last has its stdout captured and
 * handed to the next as stdin: there is no fork and nothing may block, so this is not a
 * streaming pipe, and a segment cannot start before the one before it has finished. A
 * segment that is not the last cannot change session state, as in a subshell: `exit` is
 * skipped and `cd` is undone. Returns the last segment's status. */
static int run_pipeline(sh_session_t *s, char **argv, int *quoted, int argc)
{
    char *seg[MAX_WORDS];
    int segq[MAX_WORDS];
    int start = 0, i, n, status = 0, stop = 0;

    for (i = 0; i <= argc && !stop; i++) {
        int last = (i == argc);

        if (!last && !is_pipe_word(argv[i], quoted[i])) {
            continue;
        }
        n = i - start;
        memcpy(seg, argv + start, n * sizeof(*seg));
        memcpy(segq, quoted + start, n * sizeof(*segq));
        if (!last) {
            free(argv[i]);
        }
        start = i + 1;

        if (sh_session_interrupted(s)) {
            free_args(seg, n);
            sh_session_stdin_end(s);
            status = 130;
            stop = 1;
        } else if (last) {
            status = run_command(s, seg, segq, n);
            sh_session_stdin_end(s);
        } else if (strcmp(seg[0], "exit") == 0) {
            free_args(seg, n);
            sh_session_stdin_end(s);
            status = 0;
        } else {
            sh_fsctx_t fs;
            char cwd[SH_POSIX_PATH_MAX];
            char name[NAME_MAX + 1];

            fs_of(s, &fs);
            snprintf(cwd, sizeof(cwd), "%s", fs.cwd);
            snprintf(name, sizeof(name), "%s", seg[0]);
            sh_session_capture_start(s);
            status = run_command(s, seg, segq, n);
            sh_session_stdin_end(s); /* before capture_end hands the output over as the next stdin */
            sh_session_set_cwd(s, cwd);
            if (sh_session_capture_end(s) != 0) {
                sh_eprintf(s, "sh: %s: output too large for a pipe (limit %d bytes)\n",
                           name, SSHD_STDIN_MAX);
                status = 1;
                stop = 1;
            }
        }
    }
    if (start < argc) {
        free_args(argv + start, argc - start);
    }
    return status;
}

/* One chain item: a pipeline when it holds an unquoted `|`, otherwise a single command.
 * Takes ownership of the words. */
static int run_item(sh_session_t *s, char **argv, int *quoted, int argc)
{
    int i, pipes = 0;

    for (i = 0; i < argc; i++) {
        pipes += is_pipe_word(argv[i], quoted[i]);
    }
    if (pipes == 0) {
        return run_command(s, argv, quoted, argc);
    }
    if (has_empty_segment(argv, quoted, argc)) {
        sh_eprintf(s, "sh: syntax error near unexpected token `|'\n");
        free_args(argv, argc);
        return 2;
    }
    return run_pipeline(s, argv, quoted, argc);
}

/* Runs the items of a line joined with ; && and || left to right with equal precedence, so
 * `a && b || c` is `(a && b) || c`. They bind looser than `|`: each item is a whole pipeline.
 * A skipped item leaves the status of the last item that ran. Every item's `|` is checked
 * before the first one runs, so a bad line runs nothing. An `exit` item ends the session, and
 * sh_session_interrupted() then stops the chain. Takes ownership of the words and returns the
 * status of the last item that ran. */
static int run_chain(sh_session_t *s, char **argv, int *quoted, int argc)
{
    char *item[MAX_WORDS];
    int itemq[MAX_WORDS];
    int start = 0, i, n, status = 0, stop = 0;
    sh_chain_op_t before = SH_CHAIN_SEQ;

    for (i = 0; i <= argc; i++) {
        if ((i == argc || sh_chain_op(argv[i], quoted[i]) != SH_CHAIN_NONE) &&
            has_empty_segment(argv + start, quoted + start, i - start)) {
            sh_eprintf(s, "sh: syntax error near unexpected token `|'\n");
            free_args(argv, argc);
            return 2;
        }
        if (i < argc && sh_chain_op(argv[i], quoted[i]) != SH_CHAIN_NONE) {
            start = i + 1;
        }
    }

    start = 0;
    for (i = 0; i <= argc && !stop; i++) {
        sh_chain_op_t op = i < argc ? sh_chain_op(argv[i], quoted[i]) : SH_CHAIN_NONE;

        if (i < argc && op == SH_CHAIN_NONE) {
            continue;
        }
        n = i - start;
        memcpy(item, argv + start, n * sizeof(*item));
        memcpy(itemq, quoted + start, n * sizeof(*itemq));
        if (i < argc) {
            free(argv[i]);
        }
        start = i + 1;

        if (sh_session_interrupted(s)) {
            free_args(item, n);
            stop = 1;
        } else if (!sh_chain_should_run(before, status)) {
            free_args(item, n);
        } else {
            status = run_item(s, item, itemq, n);
        }
        before = op;
    }
    if (start < argc) {
        free_args(argv + start, argc - start);
    }
    return status;
}

int sh_execute_line(sh_session_t *s, const char *line)
{
    char *argv[MAX_WORDS];
    int quoted[MAX_WORDS];
    sh_chain_op_t bad = SH_CHAIN_NONE;
    int argc, items, n;

    argc = split_line(line, argv, quoted, MAX_WORDS);
    items = sh_chain_check(argv, quoted, argc, &bad);
    if (items == SH_CHAIN_SYNTAX) {
        sh_eprintf(s, "sh: syntax error near unexpected token `%s'\n", sh_chain_op_name(bad));
        free_args(argv, argc);
        return 2;
    }
    if (items == SH_CHAIN_TOO_LONG) {
        sh_eprintf(s, "sh: too many commands on one line (limit %d)\n", SH_CHAIN_MAX);
        free_args(argv, argc);
        return 2;
    }
    n = sh_chain_words(argv, quoted, argc);
    if (n < argc) {
        free(argv[n]); /* the trailing ; */
        argc = n;
    }
    if (items == 1) {
        return run_item(s, argv, quoted, argc);
    }
    return run_chain(s, argv, quoted, argc);
}
