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

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/param.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include <psp2/io/devctl.h>

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
    if (t->kind == SH_TARGET_ROOT || t->dev == NULL || !t->dev->writable) {
        sh_eprintf(s, "%s: %s: Read-only file system\n", cmd, arg ? arg : ".");
        return -1;
    }
    return 0;
}

static int stat_target(const sh_target_t *t, struct stat *st)
{
    return stat(t->sce, st);
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
        if (strcmp(dirpart, "/") == 0) {
            return 0; /* never expand against the synthetic root */
        }
        base = dirpart;
        if (sh_path_sce(fs, dirpart, sce_dir, sizeof(sce_dir)) != 0) {
            return 0;
        }
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

static void print_long(sh_session_t *s, const char *sce_path,
                       const char *display, const sh_device_t *dev)
{
    struct stat st;
    char perms[11], when[32], size[16];

    if (stat(sce_path, &st) != 0) {
        sh_printf(s, "?  %s\n", display);
        return;
    }
    format_time(when, sizeof(when), st.st_mtime, "%b %e %H:%M");
    human_size(size, sizeof(size), (long long)st.st_size);
    sh_printf(s, "%s %6s %s  %s%s\n",
              mode_string(synth_mode(&st, dev), perms), size, when,
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

    dir = opendir(t->sce);
    if (dir == NULL) {
        sh_eprintf(s, "%s: %s: %s\n", cmd, t->posix, strerror(errno));
        return 1;
    }
    names = malloc((size_t)MAX_LIST * (NAME_MAX + 1));
    if (names == NULL) {
        closedir(dir);
        sh_eprintf(s, "%s: out of memory\n", cmd);
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
            print_long(s, sce, names[i], t->dev);
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
        return (t.kind == SH_TARGET_ROOT)
                   ? list_root(s)
                   : list_one(s, "ls", &t, long_format, show_hidden);
    }
    for (i = 0; i < paths; i++) {
        sh_target_t t;
        struct stat st;

        if (resolve_arg(s, "ls", targets[i], &t) != 0) {
            status = 1;
            continue;
        }
        if (t.kind == SH_TARGET_ROOT) {
            if (paths > 1) {
                sh_printf(s, "%s:\n", targets[i]);
            }
            list_root(s);
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
            if (list_one(s, "ls", &t, long_format, show_hidden) != 0) {
                status = 1;
            }
            continue;
        }
        /* A file argument prints as one line, like ls(1): the name, or the
         * long form when -l was asked for. */
        if (long_format) {
            print_long(s, t.sce, sh_basename(t.posix), t.dev);
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

    if (argc < 2) {
        sh_eprintf(s, "cat: missing operand\n");
        return 1;
    }
    buf = malloc(SSHD_COPY_BUFSIZE);
    if (buf == NULL) {
        sh_eprintf(s, "cat: out of memory\n");
        return 1;
    }
    for (i = 1; i < argc; i++) {
        sh_target_t t;
        ssize_t got;
        int fd;

        if (resolve_arg(s, "cat", argv[i], &t) != 0) {
            status = 1;
            continue;
        }
        if (t.kind == SH_TARGET_ROOT) {
            sh_eprintf(s, "cat: %s: Is a directory\n", argv[i]);
            status = 1;
            continue;
        }
        fd = open(t.sce, O_RDONLY);
        if (fd < 0) {
            sh_eprintf(s, "cat: %s: %s\n", argv[i], strerror(errno));
            status = 1;
            continue;
        }
        while ((got = read(fd, buf, SSHD_COPY_BUFSIZE)) > 0 &&
               !sh_session_interrupted(s)) {
            sh_write(s, buf, (size_t)got);
        }
        close(fd);
        if (got < 0) {
            sh_eprintf(s, "cat: %s: %s\n", argv[i], strerror(errno));
            status = 1;
        }
    }
    free(buf);
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
 * POSIX spellings the user typed or would recognise, for errors. cmd names the
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

    if (dst->kind == SH_TARGET_ROOT || stat(dst->sce, &st) != 0 ||
        !S_ISDIR(st.st_mode)) {
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
        format_time(when, sizeof(when), st.st_mtime, "%Y-%m-%d %H:%M:%S");
        sh_printf(s, "  File: %s\n", t.posix);
        sh_printf(s, "  Size: %-14lld  Type: %s\n", (long long)st.st_size,
                  S_ISDIR(st.st_mode) ? "directory" : "regular file");
        /* uid and gid are always 0 and always displayed as vita: the Vita has
         * no account database, so this is a display convention, not a claim.
         * The access line shows the synthesized mode, not the zero bits stat
         * hands back -- same reason as in ls -l. */
        sh_printf(s, "Access: (%04o/%s)  Uid: (0/vita)  Gid: (0/vita)\n",
                  (unsigned)(synth_mode(&st, t.dev) & 07777),
                  mode_string(synth_mode(&st, t.dev), perms));
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

static const struct {
    const char *name;
    const char *value;
} static_env[] = {
    /* PATH is decorative: commands come from the table below, never from a
     * search path. It exists so a program that inspects its environment sees
     * something sane -- a-Shell rewrites it for the same reason. */
    {"PATH", "/bin:/usr/bin"},
    {"SHELL", "/bin/sh"},
    {"TERM", "xterm-256color"},
    {"LANG", "C"},
    {"HOSTNAME", "vita"},
};

static int cmd_env(sh_session_t *s, int argc, char **argv)
{
    sh_fsctx_t fs;
    size_t i;

    (void)argc;
    (void)argv;
    fs_of(s, &fs);
    for (i = 0; i < sizeof(static_env) / sizeof(static_env[0]); i++) {
        sh_printf(s, "%s=%s\n", static_env[i].name, static_env[i].value);
    }
    sh_printf(s, "HOME=%s\n", fs.home);
    sh_printf(s, "PWD=%s\n", fs.cwd);
    sh_printf(s, "USER=%s\n", sh_session_user(s));
    sh_printf(s, "LOGNAME=%s\n", sh_session_user(s));
    return 0;
}

static int cmd_vpk(sh_session_t *s, int argc, char **argv)
{
    sh_target_t t;

    if (argc != 3 || (strcmp(argv[1], "list") != 0 && strcmp(argv[1], "install") != 0)) {
        sh_eprintf(s, "usage: vpk list FILE | vpk install FILE\n");
        return 2;
    }
    if (resolve_arg(s, "vpk", argv[2], &t) != 0) {
        return 1;
    }
    if (t.kind == SH_TARGET_ROOT || t.dev == NULL) {
        sh_eprintf(s, "vpk: %s: Not a file\n", argv[2]);
        return 1;
    }
    return (argv[1][0] == 'l') ? sh_vpk_list(s, t.sce, argv[2])
                               : sh_vpk_install(s, t.sce, argv[2]);
}

static int cmd_which(sh_session_t *s, int argc, char **argv)
{
    int i, status = 0;

    if (argc < 2) {
        sh_eprintf(s, "usage: which COMMAND...\n");
        return 1;
    }
    for (i = 1; i < argc; i++) {
        /* PATH is decorative (see static_env): everything this shell can run
         * is in the builtin table, so "found" and "builtin" are the same
         * event. Saying so beats the traditional "in (/bin:/usr/bin)", which
         * would claim a search that would find nothing. */
        if (sh_builtin_lookup(argv[i]) != NULL) {
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

static int cmd_uname(sh_session_t *s, int argc, char **argv)
{
    if (argc > 1 && strchr(argv[1], 'a') != NULL) {
        sh_printf(s, "Vita vita 3.73 VitaSDK armv7a\n");
        return 0;
    }
    sh_printf(s, "Vita\n");
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

static int cmd_help(sh_session_t *s, int argc, char **argv)
{
    const sh_builtin_t *table;
    size_t count, i;

    table = sh_builtins(&count);
    if (argc > 1) {
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
                 " `/`: `/` lists the devices, `/ux0` is the memory card.\n");
    return 0;
}

static int cmd_exit(sh_session_t *s, int argc, char **argv)
{
    (void)argc;
    (void)argv;
    sh_session_request_exit(s);
    return 0;
}

/* ------------------------------------------------------------------ */
/* the table                                                            */
/* ------------------------------------------------------------------ */

/* Keep this alphabetical: `help` prints it in order and people scan it. */
static const sh_builtin_t builtins[] = {
    {"cat", cmd_cat, "cat FILE... -- write a file to the terminal"},
    {"cd", cmd_cd, "cd [DIR] -- change the working directory (no argument: $HOME)"},
    {"cp", cmd_cp, "cp [-r] SRC... DST -- copy; -r for directories"},
    {"date", cmd_date, "date -- print the current date and time"},
    {"devices", cmd_devices, "devices -- every Vita device and whether it is mounted"},
    {"df", cmd_df, "df [DEVICE] -- free and total space per device"},
    {"du", cmd_du, "du [PATH] -- total size of a tree"},
    {"echo", cmd_echo, "echo [-n] TEXT... -- print its arguments"},
    {"env", cmd_env, "env -- print the environment"},
    {"exit", cmd_exit, "exit -- end this session"},
    {"help", cmd_help, "help [COMMAND] -- usage"},
    {"history", cmd_history, "history [COUNT] -- show numbered command history (!n re-runs line n)"},
    {"ls", cmd_ls, "ls [-l] [-a] [PATH] -- list a directory (no argument: .)"},
    {"mkdir", cmd_mkdir, "mkdir [-p] DIR... -- create directories"},
    {"mv", cmd_mv, "mv SRC... DST -- rename, copying across devices"},
    {"pwd", cmd_pwd, "pwd -- print the working directory"},
    {"rm", cmd_rm, "rm [-r] [-f] PATH... -- delete; -r for directories"},
    {"rmdir", cmd_rmdir, "rmdir DIR... -- remove an empty directory"},
    {"stat", cmd_stat, "stat PATH... -- size, mode and modification time"},
    {"touch", cmd_touch, "touch FILE... -- create a file or update its time"},
    {"uname", cmd_uname, "uname [-a] -- system name"},
    {"vpk", cmd_vpk, "vpk list FILE | vpk install FILE -- inspect or install a VPK over this app"},
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
        /* The caller gets the filename and frees it: the two shifts below
         * close BOTH slots (operator and name). Shifting once instead left
         * the filename alive as a plain argument, and `echo hello > f.txt`
         * printed "hello f.txt" into f.txt. */
        *path = argv[i + 1];
        free(argv[i]);
        {
            int j, k;

            for (k = 0; k < 2; k++) {
                for (j = i; j < *argc - 1; j++) {
                    argv[j] = argv[j + 1];
                    quoted[j] = quoted[j + 1];
                }
                (*argc)--;
            }
        }
        i--; /* the shifted-down tail may hold another redirect */
    }
    return 0;
}

int sh_execute_line(sh_session_t *s, const char *line)
{
    char *argv[MAX_WORDS];
    int quoted[MAX_WORDS];
    int argc, i, status = 0;
    const sh_builtin_t *cmd;
    char *redir_path = NULL; /* owns the filename word extract_redirect took
                              * out of argv; freed on every path below */
    int redir_append = 0;

    argc = split_line(line, argv, quoted, MAX_WORDS);
    if (argc == 0) {
        return 0;
    }
    /* Redirection words leave the list FIRST, so the glob loop never sees
     * them: a redirect target is a file to create, not a pattern to expand. */
    if (extract_redirect(s, &argc, argv, quoted, &redir_path,
                         &redir_append) != 0) {
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
            free(redir_path);
            free_args(argv, argc);
            return 1;
        }
        if (t.kind == SH_TARGET_ROOT || t.kind == SH_TARGET_DEVICE) {
            sh_eprintf(s, "sh: %s: Is a directory\n", redir_path);
            free(redir_path);
            free_args(argv, argc);
            return 1;
        }
        if (t.dev != NULL && !t.dev->writable) {
            sh_eprintf(s, "sh: %s: read-only file system\n", redir_path);
            free(redir_path);
            free_args(argv, argc);
            return 1;
        }
        if (sh_session_redirect_start(s, t.sce, redir_append) != 0) {
            sh_eprintf(s, "sh: %s: %s\n", redir_path, strerror(errno));
            free(redir_path);
            free_args(argv, argc);
            return 1;
        }
    }

    cmd = sh_builtin_lookup(argv[0]);
    if (cmd == NULL) {
        sh_eprintf(s, "%s: command not found\n", argv[0]);
        status = 127;
    } else {
        status = cmd->fn(s, argc, argv);
    }
    if (redir_path != NULL) {
        sh_session_redirect_end(s);
    }
    free(redir_path);
    free_args(argv, argc);
    return status;
}
