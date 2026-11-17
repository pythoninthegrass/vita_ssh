#ifndef VPK_UPDATE_H
#define VPK_UPDATE_H

/* Shared on-disk handoff contract. Neither VitaShell's pkg nor the credential directory. */
#ifndef VPK_UPDATE_ROOT
#define VPK_UPDATE_ROOT "ux0:data/vitasshd-update"
#endif
#define VPK_UPDATE_PKG VPK_UPDATE_ROOT "/server"
#define VPK_UPDATE_HELPER VPK_UPDATE_ROOT "/helper"
#define VPK_UPDATE_LOG VPK_UPDATE_ROOT "/result.txt"
#define VPK_UPDATE_READY VPK_UPDATE_ROOT "/ready"

#ifndef SSHD_TITLEID
#error "SSHD_TITLEID must be supplied by the build"
#endif
#ifndef SSHD_UPDATER_TITLEID
#error "SSHD_UPDATER_TITLEID must be supplied by the build"
#endif

int vpk_promote(const char *path, const char **stage);

#endif
