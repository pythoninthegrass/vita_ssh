/*
 * System information for the shell: the firmware the device reports, and the
 * strings built from it.
 *
 * The reading half is two kernel calls; the formatting half is pure and reaches
 * no Vita type, which is what lets scripts/test_sysinfo.py compile this file on
 * the host and assert on every emitted line. Keeping both in one unit is the
 * point: `uname -a` and `cat /etc/os-release` cannot report different versions,
 * because there is exactly one place a version is produced.
 */
#include "sshd_sysinfo.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <vitasdk/build_utils.h>
#include <psp2/kernel/modulemgr.h>
#include <psp2/kernel/sysmem.h>

/* The encoding is what pins the release, not a device: sceKernelGetSystemSwVersion
 * reports 0x0MMNPP00, so 0x03740000 is 3.74. versionString in the same struct is
 * not in the dotted form, so nothing here reads it, and the SDK header warns that a
 * spoofed firmware reports the spoofed value -- the number is what the kernel tells
 * any caller, which is still worth more than a literal. */
static unsigned int read_firmware(unsigned int *raw)
{
    SceKernelSystemSwVersion info;

    memset(&info, 0, sizeof(info));
    info.size = sizeof(info);
    if (sceKernelGetSystemSwVersion(&info) < 0) {
        return 0;
    }
    *raw = info.version;
    return info.version;
}

static int read_is_vitatv(void)
{
    return sceKernelGetModel() == SCE_KERNEL_MODEL_VITATV;
}

/* sceKernelGetSystemSwVersion is defined by libSceKernelModulemgr_stub.a rather
 * than the SceLibKernel_stub the server already linked, which is why
 * CMakeLists.txt adds it to the server. */
VITASDK_BUILD_ASSERT_EQ(SSHD_FW_INFO_SIZE, SceKernelSystemSwVersion);

const char *sh_sysinfo_build_stamp(void)
{
    /* The stamp sshd.log reports when the first SFTP session starts, so a device
     * run can tell two builds apart without a version field to maintain. */
    return __DATE__ " " __TIME__;
}

/* The 0x0MMNPP00 word read as it is defined: major from MM, minor from NN,
 * hundreds from PP. A word with no printable part is the honest unknown rather
 * than a plausible-looking number, since an uncorroborable version is the exact
 * bug this unit exists to remove. */
void sh_sysinfo_format_firmware(unsigned int version, char *dst, size_t dstlen)
{
    unsigned int major = (version >> 24) & 0xFFu;
    unsigned int minor = (version >> 20) & 0x0Fu;
    unsigned int hundreds = (version >> 16) & 0x0Fu;

    if (major == 0 || major > 9 || minor == 0) {
        /* Two-digit majors would need two digits of minor, which the encoding does
         * not carry, so such a word is not a firmware worth printing. */
        snprintf(dst, dstlen, "unknown");
        return;
    }
    snprintf(dst, dstlen, "%u.%u", major, minor * 10 + hundreds);
}

/* The assignment lines in the freedesktop os-release form: one per line, value
 * quoted unless it is bare digits, letters, '_' or '-'. Nothing here is derived
 * from the device except the firmware, the model and the build stamp, and the two
 * project URLs are literals so no device run can decorate them. */
int sh_sysinfo_os_release(const sh_sysinfo_t *info, char *dst, size_t dstlen)
{
    char block[SSHD_OS_RELEASE_MAX];
    int wrote;

    if (info == NULL || dst == NULL || dstlen == 0) {
        errno = EINVAL;
        return -1;
    }
    wrote = snprintf(block, sizeof(block),
                     "NAME=\"PlayStation Vita\"\n"
                     "VERSION=\"%s\"\n"
                     "ID=%s\n"
                     "ID_LIKE=vitasdk\n"
                     "VERSION_ID=\"%s\"\n"
                     "BUILD_ID=\"%s\"\n"
                     "PRETTY_NAME=\"PlayStation Vita %s\"\n"
                     "VITA_MODEL=%s\n"
                     "VITA_SSHD_BUILD=\"%s\"\n"
                     "HOME_URL=\"" SSHD_SYSINFO_HOME_URL "\"\n"
                     "SUPPORT_URL=\"" SSHD_SYSINFO_SUPPORT_URL "\"\n",
                     info->firmware,
                     info->vitatv ? "psvtv" : "psvita",
                     info->firmware,
                     info->known ? info->build_id : "unknown",
                     info->firmware,
                     info->vitatv ? "vitatv" : "vita",
                     sh_sysinfo_build_stamp());
    if (wrote < 0 || (size_t)wrote >= sizeof(block)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    if ((size_t)wrote + 1 > dstlen) {
        errno = ENAMETOOLONG;
        return -1;
    }
    /* The whole block or none of it: a caller that guessed a small buffer must not
     * be handed half an os-release. */
    memcpy(dst, block, (size_t)wrote + 1);
    return 0;
}

void sh_sysinfo_get(sh_sysinfo_t *info)
{
    static sh_sysinfo_t cached;
    static int read_once;
    unsigned int raw = 0;
    unsigned int version;

    if (info == NULL) {
        return;
    }
    if (!read_once) {
        version = read_firmware(&raw);
        sh_sysinfo_format_firmware(version, cached.firmware,
                                   sizeof(cached.firmware));
        cached.known = (cached.firmware[0] != 'u') ? 1 : 0;
        if (cached.known) {
            snprintf(cached.build_id, sizeof(cached.build_id), "0x%08X", raw);
        } else {
            snprintf(cached.build_id, sizeof(cached.build_id), "unknown");
        }
        cached.vitatv = read_is_vitatv();
        read_once = 1;
    }
    *info = cached;
}
