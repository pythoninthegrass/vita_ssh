/* The one source of system information for uname and /etc/os-release; the formatter is compiled on the host by scripts/test_sysinfo.py. */
#ifndef SSHD_SYSINFO_H
#define SSHD_SYSINFO_H

#include <stddef.h>

/* Project URLs, literals so no device run can decorate them. */
#define SSHD_SYSINFO_HOME_URL "https://www.reddit.com/r/vitahacks"
#define SSHD_SYSINFO_SUPPORT_URL "https://github.com/pythoninthegrass/vita_ssh"

/* sizeof(SceKernelSystemSwVersion), spelled out for the host check; the device build asserts it against the real struct. */
#define SSHD_FW_INFO_SIZE 0x28

/* What the shell knows about this device, already turned into strings. */
typedef struct {
    char firmware[16]; /* dotted release, "unknown" when the query failed */
    char build_id[20]; /* the raw 0x0MMNPP00 word, "unknown" with no firmware */
    int known;         /* 0 when the firmware query failed */
    int vitatv;        /* 1 for a TV, 0 for a handheld */
} sh_sysinfo_t;

/* Reads the firmware and model once and keeps the result for the process. */
void sh_sysinfo_get(sh_sysinfo_t *info);

/* Formats a firmware word: "3.74" for 0x03740000, "unknown" for a word with no printable part. */
void sh_sysinfo_format_firmware(unsigned int version, char *dst, size_t dstlen);

/* The compile stamp, the same one sshd.log reports when an SFTP session starts. */
const char *sh_sysinfo_build_stamp(void);

/* Worst-case size of the os-release block. */
#define SSHD_OS_RELEASE_MAX 1024

/* Writes the os-release block; returns 0, or -1 with errno set and dst untouched when dst is too small. */
int sh_sysinfo_os_release(const sh_sysinfo_t *info, char *dst, size_t dstlen);

#endif /* SSHD_SYSINFO_H */
