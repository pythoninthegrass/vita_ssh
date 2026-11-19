/* POSIX surfaces libssh references that the Vita platform does not have.
 *
 * Every symbol here came out of the link step as an undefined reference, and
 * every one is a genuine platform absence rather than a missing port: Vita has
 * no processes to wait on or replace, no uid/gid database, and no symlinks.
 * None is on the path a server session takes -- they live in the client-side
 * ProxyCommand/ProxyJump code and in the identity lookups libssh performs when
 * a caller passes a "~" or a username. Failing them loudly is the correct
 * answer; pretending otherwise is how you get a server that authenticates a
 * user and then hangs.
 *
 * The genuinely useful platform calls -- open/read/write/close/lseek/stat/
 * lstat/opendir/readdir/mkdir/rmdir/unlink/rename, socket/bind/listen/accept/
 * recv/send, and poll/select -- are NOT here: VitaSDK's newlib already maps
 * those onto sceIo* and sceNet*.
 */
#include <errno.h>
#include <grp.h>
#include <pwd.h>
#include <stddef.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define UNSUPPORTED(what) (errno = ENOSYS, (void)(what), -1)

/* --- process model: used only by ProxyCommand / ProxyJump, never by a server --- */
int dup2(int oldfd, int newfd)
{
    (void)oldfd; (void)newfd;
    return UNSUPPORTED("dup2");
}

pid_t waitpid(pid_t pid, int *status, int options)
{
    (void)pid; (void)status; (void)options;
    return UNSUPPORTED("waitpid");
}

int execv(const char *path, char *const argv[])
{
    (void)path; (void)argv;
    return UNSUPPORTED("execv");
}

/* --- identity: libssh uses these to expand "~" and to fill ls -l style names --- */
int getpwuid_r(uid_t uid, struct passwd *pwd, char *buf, size_t buflen, struct passwd **result)
{
    (void)uid; (void)buf; (void)buflen;
    if (pwd != NULL) {
        *pwd = (struct passwd){ 0 };
    }
    if (result != NULL) {
        *result = NULL; /* no such user, not an error: callers handle NULL */
    }
    return 0;
}

struct passwd *getpwnam(const char *name)
{
    (void)name;
    errno = ENOENT;
    return NULL;
}

int getgrgid_r(gid_t gid, struct group *grp, char *buf, size_t buflen, struct group **result)
{
    (void)gid; (void)buf; (void)buflen;
    if (grp != NULL) {
        *grp = (struct group){ 0 };
    }
    if (result != NULL) {
        *result = NULL;
    }
    return 0;
}

/* --- symlinks: no such object on the Vita filesystem --- */
int symlink(const char *target, const char *linkpath)
{
    (void)target; (void)linkpath;
    return UNSUPPORTED("symlink");
}

int readlink(const char *path, char *buf, size_t bufsiz)
{
    (void)path; (void)buf; (void)bufsiz;
    return UNSUPPORTED("readlink");
}

// Anchors the read-only output section that cmake/vita_seg0_slack.ld pads to a page boundary.
__attribute__((section(".vita_seg0_slack"), used)) static const char seg0_slack_anchor = 0;
