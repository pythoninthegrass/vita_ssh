/* System information for the shell: the firmware the device reports and the strings built from it, in one place so uname and os-release cannot disagree. */
#include "sshd_sysinfo.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <vitasdk/build_utils.h>
#include <psp2/kernel/modulemgr.h>
#include <psp2/kernel/sysmem.h>

/* Reads the 0x0MMNPP00 firmware word (0x03740000 is 3.74); a spoofed firmware reports the spoofed value, and 0 means the query failed. */
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

/* sceKernelGetSystemSwVersion lives in libSceKernelModulemgr_stub.a, which CMakeLists.txt adds to the server. */
VITASDK_BUILD_ASSERT_EQ(SSHD_FW_INFO_SIZE, SceKernelSystemSwVersion);

const char *sh_sysinfo_build_stamp(void)
{
    /* The stamp sshd.log reports when the first SFTP session starts. */
    return __DATE__ " " __TIME__;
}

/* Formats the 0x0MMNPP00 word as major.minor; a word with no printable part gives "unknown", never a plausible number. */
void sh_sysinfo_format_firmware(unsigned int version, char *dst, size_t dstlen)
{
    unsigned int major = (version >> 24) & 0xFFu;
    unsigned int minor = (version >> 20) & 0x0Fu;
    unsigned int hundreds = (version >> 16) & 0x0Fu;

    if (major == 0 || major > 9 || minor == 0) {
        /* The encoding carries one digit of major, so anything else is not a firmware. */
        snprintf(dst, dstlen, "unknown");
        return;
    }
    snprintf(dst, dstlen, "%u.%u", major, minor * 10 + hundreds);
}

/* Emits the os-release assignments; only firmware, model and build stamp come from the device, the two project URLs are literals. */
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
    /* The whole block or none of it. */
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
