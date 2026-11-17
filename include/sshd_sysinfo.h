/*
 * The one source of system information for the shell.
 *
 * `uname -a` and `cat /etc/os-release` both come through here rather than each
 * holding its own copy of the version, because the day they disagree is the day
 * a bug report quotes a firmware number the device never reported. One kernel
 * call reads the firmware, one pure formatter turns it into strings, and that
 * formatter is compiled on the host by scripts/test_sysinfo.py.
 */
#ifndef SSHD_SYSINFO_H
#define SSHD_SYSINFO_H

#include <stddef.h>

/* The two fields that describe the project rather than the device are written as
 * literals, in one place, so no device run can decorate them and the host check
 * can assert on them without duplicating them. */
#define SSHD_SYSINFO_HOME_URL "https://www.reddit.com/r/vitahacks"
#define SSHD_SYSINFO_SUPPORT_URL "https://github.com/pythoninthegrass/vita_ssh"

/* What sceKernelGetSystemSwVersion wants back as SceSize, spelled here because
 * scripts/test_sysinfo.py compiles src/sshd_sysinfo.c with stub headers rather than
 * the SDK's. The device build asserts it against the real struct. */
#define SSHD_FW_INFO_SIZE 0x28

/* What the shell knows about this device, already turned into strings. */
typedef struct {
    char firmware[16]; /* dotted release, "unknown" when the query failed */
    char build_id[20]; /* the raw 0x0MMNPP00 word, "unknown" with no firmware */
    int known;         /* 0 when the firmware query failed */
    int vitatv;        /* 1 for a TV, 0 for a handheld */
} sh_sysinfo_t;

/* Reads the firmware and the model once and keeps the result for the process. One
 * read also means one answer: no command can see a firmware change halfway through
 * a session and start disagreeing with the other surface. */
void sh_sysinfo_get(sh_sysinfo_t *info);

/* The firmware word formatted: "3.74" for 0x03740000, "unknown" for a word with
 * no printable part. */
void sh_sysinfo_format_firmware(unsigned int version, char *dst, size_t dstlen);

/* The compile stamp, the same one sshd.log reports when an SFTP session starts. */
const char *sh_sysinfo_build_stamp(void);

/* Bytes sh_sysinfo_os_release() needs for the worst case, so a caller never has to
 * guess and a short buffer is never the reason a field went missing. */
#define SSHD_OS_RELEASE_MAX 1024

/* The whole os-release block, assignments only. Returns 0, or -1 with errno set
 * and dst untouched when it is too small. */
int sh_sysinfo_os_release(const sh_sysinfo_t *info, char *dst, size_t dstlen);

#endif /* SSHD_SYSINFO_H */
