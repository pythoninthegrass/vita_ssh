/* A separate title: the promoter cannot replace a title while it is running. */
#include "vpk_update.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <psp2/appmgr.h>
#include <psp2/kernel/processmgr.h>

int main(void)
{
    const char *stage = "no pending update";
    FILE *fp;
    char title[16] = {0};
    int res = -1;

    /* A manually launched helper must not replay a previous installation. */
    fp = fopen(VPK_UPDATE_READY, "rb");
    if (fp != NULL) {
        size_t n = fread(title, 1, sizeof(title) - 1, fp);
        fclose(fp);
        if (unlink(VPK_UPDATE_READY) == 0 &&
            n == strlen(SSHD_TITLEID) && strcmp(title, SSHD_TITLEID) == 0) {
            /* Target only our server, never unrelated running apps. */
            stage = "stop server";
            SceUID pid;
            res = sceAppMgrGetIdByName(&pid, SSHD_TITLEID);
            if (res >= 0) {
                res = sceAppMgrDestroyAppByName(SSHD_TITLEID);
            } else {
                res = 0; /* The server normally exits immediately after launch. */
            }
            if (res >= 0) {
                res = vpk_promote(VPK_UPDATE_PKG, &stage);
            }
        }
    }
    fp = fopen(VPK_UPDATE_LOG, "wb");
    if (fp != NULL) {
        fprintf(fp, "%s: 0x%08X\n", stage, (unsigned)res);
        fclose(fp);
    }
    /* On installer failure, launch the old server too. Keep stage + result for diagnosis. */
    res = sceAppMgrLaunchAppByUri(0xFFFFF, "psgm:play?titleid=" SSHD_TITLEID);
    if (res < 0) {
        fp = fopen(VPK_UPDATE_LOG, "ab");
        if (fp != NULL) {
            fprintf(fp, "launch server: 0x%08X\n", (unsigned)res);
            fclose(fp);
        }
    }
    sceKernelExitProcess(0);
    return 0;
}
