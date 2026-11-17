/* Host test for the pure half of src/sshd_sysinfo.c. scripts/test_sysinfo.py compiles
 * it and passes the two project URLs and the required field names as -D flags, so the
 * literals this check asserts against live in exactly one place. */
#include "sh_path.h"
#include "sshd_sysinfo.h"

#include <errno.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
            failures++;                                                 \
        }                                                               \
    } while (0)

#define MAX_LINES 32
#define LINE_BUF 512

/* What cmd_uname prints for -a, mirrored here so the two surfaces are compared the
 * way a reader on a device compares them. */
static void uname_line(const sh_sysinfo_t *info, char *dst, size_t dstlen)
{
    snprintf(dst, dstlen, "Vita vita %s VitaSDK %s\n", info->firmware,
             info->vitatv ? "armv7a (vitatv)" : "armv7a");
}

static sh_sysinfo_t make(unsigned int version, int known, int vitatv)
{
    sh_sysinfo_t info;

    memset(&info, 0, sizeof(info));
    info.known = known;
    info.vitatv = vitatv;
    if (known) {
        sh_sysinfo_format_firmware(version, info.firmware, sizeof(info.firmware));
        snprintf(info.build_id, sizeof(info.build_id), "0x%08X", version);
    } else {
        snprintf(info.firmware, sizeof(info.firmware), "unknown");
        snprintf(info.build_id, sizeof(info.build_id), "unknown");
    }
    return info;
}

static char lines[LINE_BUF];
static char *line_ptrs[MAX_LINES];

/* Splits a block into lines; the buffer is owned here, reused per call. */
static int split_lines(const char *block)
{
    size_t n = 0, start = 0, i;

    for (i = 0;; i++) {
        if (block[i] == '\n' || block[i] == '\0') {
            if (i > start) {
                if (n == MAX_LINES) {
                    return -1;
                }
                memcpy(&lines[start], &block[start], i - start);
                lines[i] = '\0';
                line_ptrs[n++] = &lines[start];
            }
            start = i + 1;
            if (block[i] == '\0') {
                break;
            }
        }
    }
    return (int)n;
}

static int has_line(int n, const char *want)
{
    int i;

    for (i = 0; i < n; i++) {
        if (strcmp(line_ptrs[i], want) == 0) {
            return 1;
        }
    }
    return 0;
}

/* One assignment per line, valid identifier, no spaces around '='. That is the
 * form `grep '^VERSION_ID='` and `set -a; . /etc/os-release` both need. */
static int is_assignment(const char *line)
{
    const char *eq, *c;

    eq = strchr(line, '=');
    if (eq == NULL || eq == line) {
        return 0;
    }
    for (c = line; c < eq; c++) {
        if (!((*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') || *c == '_')) {
            return 0;
        }
    }
    if (*c != '=') {
        return 0;
    }
    if (eq[1] != '"') {
        /* Unquoted: os-release allows only letters, digits, '_' and '-', which also
         * rules out the spaces a bare `KEY=a b` would need quoting for. */
        for (c = eq + 1; *c != '\0'; c++) {
            if (!((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') ||
                  (*c >= '0' && *c <= '9') || *c == '_' || *c == '-')) {
                return 0;
            }
        }
        return 1;
    }
    /* Quoted: the value is anything up to a closing quote and nothing after it,
     * which is the form that survives both grep '^KEY=' and `source`. */
    c = strchr(eq + 2, '"');
    if (c == NULL) {
        return 0; /* an unterminated quote would swallow the rest of the file */
    }
    return (c[1] == '\0') ? 1 : 0;
}

/* True for the two fields that are allowed to carry a URL. */
static int is_url_line(const char *line)
{
    return strncmp(line, "HOME_URL=", 9) == 0 ||
           strncmp(line, "SUPPORT_URL=", 12) == 0;
}

static int find_prefix(int n, const char *prefix);

static int check_required(int n)
{
    static const char *required[] = {REQUIRED_FIELDS};
    char want[160];
    size_t i;
    int missing = 0;

    for (i = 0; i < sizeof(required) / sizeof(required[0]); i++) {
        snprintf(want, sizeof(want), "%s=", required[i]);
        if (find_prefix(n, want) < 0) {
            printf("FAIL missing field %s\n", required[i]);
            failures++;
            missing++;
        }
    }
    return missing == 0;
}

static int find_prefix(int n, const char *prefix)
{
    int i;
    size_t len = strlen(prefix);

    for (i = 0; i < n; i++) {
        if (strncmp(line_ptrs[i], prefix, len) == 0) {
            return i;
        }
    }
    return -1;
}

static void check_no_stray_url(const char *block)
{
    const char *p;

    for (p = block; (p = strstr(p, "http")) != NULL; p += 4) {
        if (strncmp(p, HOME_URL, strlen(HOME_URL)) != 0 &&
            strncmp(p, SUPPORT_URL, strlen(SUPPORT_URL)) != 0) {
            printf("FAIL unexpected URL near: %.40s\n", p);
            failures++;
        }
    }
}

static void check_block(unsigned int version, int vitatv)
{
    sh_sysinfo_t info = make(version, 1, vitatv);
    char block[SSHD_OS_RELEASE_MAX];
    char want[160];
    char printed[128];
    int n, i;

    CHECK(sh_sysinfo_os_release(&info, block, sizeof(block)) == 0);
    n = split_lines(block);
    CHECK(n > 0);
    for (i = 0; i < n; i++) {
        CHECK(is_assignment(line_ptrs[i]));
        /* A machine-local identifier would arrive as an address, a hostname or a
         * personal name: none of those contain '@', and only the two project URLs
         * are allowed a scheme and a host. */
        CHECK(strchr(line_ptrs[i], '@') == NULL &&
              (is_url_line(line_ptrs[i]) || strstr(line_ptrs[i], "://") == NULL));
    }
    check_required(n);

    snprintf(want, sizeof(want), "VERSION_ID=\"%s\"", info.firmware);
    CHECK(has_line(n, want));
    snprintf(want, sizeof(want), "VERSION=\"%s\"", info.firmware);
    CHECK(has_line(n, want));
    snprintf(want, sizeof(want), "NAME=\"PlayStation Vita\"");
    CHECK(has_line(n, want));
    snprintf(want, sizeof(want), "ID=%s", vitatv ? "psvtv" : "psvita");
    CHECK(has_line(n, want));
    snprintf(want, sizeof(want), "ID_LIKE=vitasdk");
    CHECK(has_line(n, want));
    snprintf(want, sizeof(want), "VITA_MODEL=%s", vitatv ? "vitatv" : "vita");
    CHECK(has_line(n, want));
    snprintf(want, sizeof(want), "BUILD_ID=\"0x%08X\"", version);
    CHECK(has_line(n, want));
    snprintf(want, sizeof(want), "PRETTY_NAME=\"PlayStation Vita %s\"",
             info.firmware);
    CHECK(has_line(n, want));
    snprintf(want, sizeof(want), "HOME_URL=\"%s\"", HOME_URL);
    CHECK(has_line(n, want));
    snprintf(want, sizeof(want), "SUPPORT_URL=\"%s\"", SUPPORT_URL);
    CHECK(has_line(n, want));
    check_no_stray_url(block);

    /* The one shared source: the release in uname -a is the release in VERSION_ID. */
    uname_line(&info, printed, sizeof(printed));
    CHECK(strstr(printed, info.firmware) != NULL);
    CHECK(find_prefix(n, "BUILD_ID=\"0x") >= 0);
}

static void check_failure_path(void)
{
    sh_sysinfo_t info = make(0u, 0, 0);
    char block[SSHD_OS_RELEASE_MAX];
    char printed[128];
    int n, i;

    CHECK(sh_sysinfo_os_release(&info, block, sizeof(block)) == 0);
    n = split_lines(block);
    CHECK(n > 0);
    for (i = 0; i < n; i++) {
        CHECK(is_assignment(line_ptrs[i]));
        /* Nothing that could be read as a firmware number. The build stamp is the
         * one field with digits in it and is not a version anyone could quote. */
        if (strncmp(line_ptrs[i], "VITA_SSHD_BUILD=", 16) != 0) {
            CHECK(strpbrk(line_ptrs[i], "0123456789") == NULL);
        }
    }
    check_required(n);
    CHECK(has_line(n, "VERSION_ID=\"unknown\""));
    CHECK(has_line(n, "VERSION=\"unknown\""));
    CHECK(has_line(n, "BUILD_ID=\"unknown\""));
    CHECK(has_line(n, "PRETTY_NAME=\"PlayStation Vita unknown\""));
    uname_line(&info, printed, sizeof(printed));
    CHECK(strstr(printed, "unknown") != NULL);
}

static void check_buffer_bounds(void)
{
    sh_sysinfo_t info = make(0x03740000u, 1, 0);
    char block[SSHD_OS_RELEASE_MAX];
    char small[16];

    CHECK(sh_sysinfo_os_release(&info, block, sizeof(block)) == 0);
    CHECK(sh_sysinfo_os_release(&info, NULL, sizeof(block)) == -1);
    CHECK(sh_sysinfo_os_release(&info, small, 0) == -1);
    memset(small, 'x', sizeof(small));
    CHECK(sh_sysinfo_os_release(&info, small, sizeof(small)) == -1);
    /* A rejected call leaves the buffer alone rather than half-writing it. */
    CHECK(memcmp(small, "xxxxxxxxxxxxxxxx", sizeof(small)) == 0);
}

static void check_two_calls_agree(void)
{
    sh_sysinfo_t a = make(0x03730000u, 1, 0);
    sh_sysinfo_t b = make(0x03730000u, 1, 0);
    char one[SSHD_OS_RELEASE_MAX], two[SSHD_OS_RELEASE_MAX];

    CHECK(sh_sysinfo_os_release(&a, one, sizeof(one)) == 0);
    CHECK(sh_sysinfo_os_release(&b, two, sizeof(two)) == 0);
    CHECK(strcmp(one, two) == 0);
    /* The block ends in exactly one newline: it is the whole file. */
    CHECK(one[strlen(one) - 1] == '\n');
    CHECK(one[strlen(one) - 2] != '\n');
}

static void check_firmware(void)
{
    char got[16];

    sh_sysinfo_format_firmware(0x03740000u, got, sizeof(got));
    CHECK(strcmp(got, "3.74") == 0);
    sh_sysinfo_format_firmware(0x03600000u, got, sizeof(got));
    CHECK(strcmp(got, "3.60") == 0);
    sh_sysinfo_format_firmware(0x03730000u, got, sizeof(got));
    CHECK(strcmp(got, "3.73") == 0);
    /* The minor nibble is 0 and the hundreds digit is 6. */
    sh_sysinfo_format_firmware(0x03650000u, got, sizeof(got));
    CHECK(strcmp(got, "3.65") == 0);
    /* Two-digit majors would need two digits of minor, which 0x0MMNPP00 does not
     * carry; such a word is not a firmware this shell claims to understand. */
    sh_sysinfo_format_firmware(0x0A010000u, got, sizeof(got));
    CHECK(strcmp(got, "unknown") == 0);
    /* No printable part in the encoding: the honest value, not a made-up one. */
    sh_sysinfo_format_firmware(0x03000000u, got, sizeof(got));
    CHECK(strcmp(got, "unknown") == 0);
    sh_sysinfo_format_firmware(0u, got, sizeof(got));
    CHECK(strcmp(got, "unknown") == 0);
}

/* The read half, against the stubbed kernel: sh_sysinfo_get() must derive the same
 * release the formatter derives from the word the kernel reported. */
static void check_read(void)
{
    const char *want_fw = getenv("VITA_READ_FIRMWARE");
    sh_sysinfo_t info;
    char block[SSHD_OS_RELEASE_MAX];
    char want[160];
    int n, i;

    sh_sysinfo_get(&info);
    CHECK(strcmp(info.firmware, want_fw) == 0);
    CHECK(info.vitatv == (getenv("VITA_READ_MODEL") != NULL &&
                          strcmp(getenv("VITA_READ_MODEL"), "vitatv") == 0));
    if (strcmp(want_fw, "unknown") == 0) {
        CHECK(!info.known);
        CHECK(strcmp(info.build_id, "unknown") == 0);
    } else {
        CHECK(info.known);
        CHECK(strcmp(info.build_id, getenv("VITA_READ_BUILD_ID")) == 0);
    }
    CHECK(sh_sysinfo_os_release(&info, block, sizeof(block)) == 0);
    n = split_lines(block);
    for (i = 0; i < n; i++) {
        CHECK(is_assignment(line_ptrs[i]));
    }
    check_required(n);
    snprintf(want, sizeof(want), "VERSION_ID=\"%s\"", want_fw);
    CHECK(has_line(n, want));
    snprintf(want, sizeof(want), "ID=%s",
             (getenv("VITA_READ_MODEL") != NULL &&
              strcmp(getenv("VITA_READ_MODEL"), "vitatv") == 0) ? "psvtv" : "psvita");
    CHECK(has_line(n, want));
    /* Cached: a second call reports what the first one read. */
    {
        sh_sysinfo_t again;

        sh_sysinfo_get(&again);
        CHECK(memcmp(&info, &again, sizeof(info)) == 0);
    }
}

/* The path layer's half of the contract: /etc/os-release resolves as a pseudo-file,
 * nothing else under /etc resolves at all, and no pseudo-file has a sceIo spelling --
 * which is what keeps /etc out of SFTP and out of every libc call. */
static void check_path_layer(void)
{
    sh_fsctx_t fs;
    sh_target_t t;
    char sce[SH_SCE_PATH_MAX];

    fs.cwd = "/ux0";
    fs.home = "/ux0/vitasshd"; /* what sh_auth_t hands out */

    /* The name people type, reached the way a builtin reaches it. */
    errno = 0;
    CHECK(sh_path_resolve(&fs, "/etc/os-release", &t) == 0);
    CHECK(t.kind == SH_TARGET_PSEUDO);
    CHECK(t.dev == NULL);
    CHECK(t.pseudo != NULL);
    CHECK(t.sce[0] == '\0');
    CHECK(strcmp(t.posix, "/etc/os-release") == 0);
    CHECK(t.pseudo->size == strlen(t.pseudo->contents));
    CHECK(t.pseudo->size > 0);
    /* No sce spelling: sh_path_sce is what every SFTP handler and libc call goes
     * through, so failing here is the whole of "SFTP cannot see /etc". */
    errno = 0;
    CHECK(sh_path_sce(&fs, "/etc/os-release", sce, sizeof(sce)) == -1);
    CHECK(errno == ENOTSUP);

    /* The pseudo-file lookup is exact: the directory is not a file. */
    CHECK(sh_pseudo_lookup("/etc") == NULL);
    CHECK(sh_pseudo_lookup("/etc/os-release") != NULL);
    CHECK(sh_pseudo_lookup(NULL) == NULL);
    /* ls /etc comes from the table, not from a resolution of "/etc". */
    {
        const char *names[8];

        CHECK(sh_pseudo_list("/etc", names, 8) == 1);
    }

    /* Any other name under /etc is as absent as a typo. */
    errno = 0;
    CHECK(sh_path_resolve(&fs, "/etc/shadow", &t) == -1);
    CHECK(errno == ENOENT);
    errno = 0;
    CHECK(sh_path_resolve(&fs, "/etc/passwd", &t) == -1);
    CHECK(errno == ENOENT);
    /* A prefix is not a match. */
    errno = 0;
    CHECK(sh_path_resolve(&fs, "/etc/os-release.x", &t) == -1);
    CHECK(errno == ENOENT);
    /* The directory is not a file: the lookup is exact, so listing /etc and
     * cat-ing /etc are different questions and only the second one lies. */
    CHECK(sh_pseudo_lookup("/etc") == NULL);
    CHECK(sh_pseudo_lookup("/etc/os-release") != NULL);
    CHECK(sh_pseudo_lookup(NULL) == NULL);

    /* Normalizing must reach the pseudo-file by every spelling that means it, and
     * must not invent one out of a walk that leaves its directory. */
    CHECK(sh_path_resolve(&fs, "/etc/./os-release", &t) == 0);
    CHECK(t.kind == SH_TARGET_PSEUDO);
    CHECK(sh_path_resolve(&fs, "//etc///os-release", &t) == 0);
    CHECK(t.kind == SH_TARGET_PSEUDO);
    /* `cd /etc` first, then the file: same answer, because ".." of a synthetic
     * directory is the directory's own parent and cannot smuggle in a name. */
    errno = 0;
    CHECK(sh_path_resolve(&fs, "/etc/../etc/./os-release", &t) == -1);
    CHECK(errno == ENOENT);
    CHECK(t.kind == SH_TARGET_ROOT);
    CHECK(t.pseudo == NULL);
    CHECK(sh_path_resolve(&fs, "~", &t) == 0);
    CHECK(t.kind == SH_TARGET_PATH); /* home is a directory inside a device */
    CHECK(sh_path_resolve(&fs, "/ETC/os-release", &t) == -1);
    CHECK(errno == ENOENT);
    /* Home is a directory on a device, so ".." from it walks that device and can
     * never land on a pseudo-file. */
    errno = 0;
    CHECK(sh_path_resolve(&fs, "~/../etc/os-release", &t) == 0);
    CHECK(t.kind == SH_TARGET_PATH);
    CHECK(t.pseudo == NULL);

    /* A device that starts with the same letters stays a device. */
    CHECK(sh_path_resolve(&fs, "/ux0/etc/os-release", &t) == 0);
    CHECK(t.kind == SH_TARGET_PATH);
    CHECK(t.pseudo == NULL);
    CHECK(strcmp(t.sce, "ux0:/etc/os-release") == 0);
    CHECK(sh_path_sce(&fs, "/ux0/etc/os-release", sce, sizeof(sce)) == 0);
    CHECK(strcmp(sce, "ux0:/etc/os-release") == 0);

    /* ".." is ordinary normalization, and it stops at the device. The device is
     * component 0 of every resolution -- typed by the caller for an absolute path,
     * taken from the cwd or home otherwise -- so no ".." can pop it and no path can
     * leave its storage area. Both halves have been got here by getting one wrong:
     *   - letting the device be popped turned `cd /ux0/data/x` followed by
     *     `../../../etc/os-release` into a walk into the synthetic tree;
     *   - pinning the cwd instead made every `cd ..` inside a subdirectory a silent
     *     no-op, because the floor sat below the components the user was standing in.
     * So the walk climbs, and the device holds. */
    fs.cwd = "/ux0/data/x";
    CHECK(sh_path_resolve(&fs, ".", &t) == 0);
    CHECK(strcmp(t.posix, "/ux0/data/x") == 0);
    CHECK(sh_path_resolve(&fs, "..", &t) == 0);
    CHECK(strcmp(t.posix, "/ux0/data") == 0);
    CHECK(strcmp(t.sce, "ux0:/data") == 0);
    CHECK(sh_path_resolve(&fs, "../..", &t) == 0);
    CHECK(t.kind == SH_TARGET_DEVICE);
    CHECK(strcmp(t.posix, "/ux0") == 0);
    CHECK(strcmp(t.sce, "ux0:") == 0);
    /* Past the device the ".." are simply spent: no error, no new device. */
    CHECK(sh_path_resolve(&fs, "../../..", &t) == 0);
    CHECK(t.kind == SH_TARGET_DEVICE);
    CHECK(strcmp(t.posix, "/ux0") == 0);
    CHECK(sh_path_resolve(&fs, "../../../../../../..", &t) == 0);
    CHECK(t.kind == SH_TARGET_DEVICE);
    CHECK(strcmp(t.posix, "/ux0") == 0);
    /* The escape that started this: from inside the memory card, a name that looks
     * exactly like the pseudo-file. It stays an ordinary file on ux0, and `cat`
     * there reports what the memory card holds rather than the firmware. */
    CHECK(sh_path_resolve(&fs, "../../../etc/os-release", &t) == 0);
    CHECK(t.kind == SH_TARGET_PATH);
    CHECK(t.pseudo == NULL);
    CHECK(strcmp(t.sce, "ux0:/etc/os-release") == 0);
    CHECK(strcmp(t.posix, "/ux0/etc/os-release") == 0);
    /* An absolute path walks its own device freely but never leaves it: the first
     * component survives every "..", so this is the memory card, not vs0. */
    fs.cwd = "/ux0";
    CHECK(sh_path_resolve(&fs, "/ux0/../../vs0/etc", &t) == 0);
    CHECK(t.kind == SH_TARGET_PATH);
    CHECK(strcmp(t.sce, "ux0:/vs0/etc") == 0);
    CHECK(sh_path_resolve(&fs, "/ux0/../../etc/os-release", &t) == 0);
    CHECK(t.pseudo == NULL);
    CHECK(strcmp(t.sce, "ux0:/etc/os-release") == 0);
    /* Home the same way: `cd ~` is the server's data directory, one ".." is its
     * device, and the device is the floor. */
    CHECK(sh_path_resolve(&fs, "~", &t) == 0);
    CHECK(strcmp(t.posix, "/ux0/vitasshd") == 0);
    CHECK(sh_path_resolve(&fs, "~/..", &t) == 0);
    CHECK(t.kind == SH_TARGET_DEVICE);
    CHECK(strcmp(t.posix, "/ux0") == 0);
    CHECK(sh_path_resolve(&fs, "~/../etc/os-release", &t) == 0);
    CHECK(t.pseudo == NULL);
    CHECK(strcmp(t.sce, "ux0:/etc/os-release") == 0);

    /* A cwd of "/" has no device to root a relative path in: a name that is not
     * a device or a pseudo-file is refused rather than landing on a storage area
     * nobody chose, while "etc" is the /etc directory as in any shell. */
    fs.cwd = "/";
    errno = 0;
    CHECK(sh_path_resolve(&fs, "foo", &t) == -1);
    CHECK(errno == ENOENT);
    CHECK(sh_path_resolve(&fs, "etc", &t) == 0);
    CHECK(t.kind == SH_TARGET_PSEUDO_DIR);
    fs.cwd = "/ux0";

    /* The device table and "/" still behave: the pseudo-files must not have joined
     * the root listing. */
    CHECK(sh_path_resolve(&fs, "/", &t) == 0);
    CHECK(t.kind == SH_TARGET_ROOT);
    CHECK(t.pseudo == NULL);

    /* /etc itself is a directory the shell can list and stat, never open. */
    errno = 0;
    CHECK(sh_path_resolve(&fs, "/etc", &t) == 0);
    CHECK(t.kind == SH_TARGET_PSEUDO_DIR);
    CHECK(t.dev == NULL);
    CHECK(t.pseudo == NULL);
    CHECK(t.sce[0] == '\0');
    CHECK(strcmp(t.posix, "/etc") == 0);
    CHECK(sh_path_resolve(&fs, "/etc/", &t) == 0);
    CHECK(t.kind == SH_TARGET_PSEUDO_DIR);
    CHECK(sh_path_resolve(&fs, "//etc/.", &t) == 0);
    CHECK(t.kind == SH_TARGET_PSEUDO_DIR);
    errno = 0;
    CHECK(sh_path_resolve(&fs, "/etcx", &t) == -1);
    CHECK(errno == ENOENT);
    errno = 0;
    CHECK(sh_path_sce(&fs, "/etc", sce, sizeof(sce)) == -1);
    CHECK(errno == ENOTSUP);
    CHECK(sh_pseudo_dir_exists("/etc") == 1);
    CHECK(sh_pseudo_dir_exists("/") == 0);
    CHECK(sh_pseudo_dir_exists("/ux0") == 0);
    CHECK(sh_pseudo_dir_exists("/etc/os-release") == 0);
    CHECK(sh_pseudo_dir_exists(NULL) == 0);

    /* ls /etc gets its names from the table, in order. */
    {
        const char *names[8];
        size_t count = sh_pseudo_list("/etc", names, 8);

        CHECK(count == 1);
        if (count == 1) {
            CHECK(strcmp(names[0], "os-release") == 0);
        }
        CHECK(sh_pseudo_list("/", names, 8) == 0);
        CHECK(sh_pseudo_list("/ux0", names, 8) == 0);
    }
}

/* A pseudo-file fd must never equal -1, the value open() returns on failure. */
static void check_pseudo_fd(void)
{
    int slot;

    for (slot = 0; slot < 8; slot++) {
        int fd = sh_pseudo_fd_encode(slot);

        CHECK(fd < -1);
        CHECK(sh_pseudo_fd_slot(fd) == slot);
    }
    CHECK(sh_pseudo_fd_slot(-1) == -1);
    CHECK(sh_pseudo_fd_slot(0) == -1);
    CHECK(sh_pseudo_fd_slot(3) == -1);
}

int main(void)
{
    const char *which = getenv("VITA_CHECK");

    if (which != NULL && strcmp(which, "read") == 0) {
        check_read();
    } else {
        check_firmware();
        check_block(0x03740000u, 0);
        check_block(0x03600000u, 0);
        check_block(0x03730000u, 1);
        check_failure_path();
        check_buffer_bounds();
        check_two_calls_agree();
        check_path_layer();
        check_pseudo_fd();
    }
    if (failures != 0) {
        printf("%d check(s) failed\n", failures);
        return 1;
    }
    printf("ok\n");
    return 0;
}
