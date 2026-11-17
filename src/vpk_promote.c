#include "vpk_update.h"

#include <stdint.h>
#include <string.h>
#include <psp2/promoterutil.h>
#include <psp2/sysmodule.h>

static int paf_load(void)
{
    static uint32_t argp[] = {0x180000, (uint32_t)-1, (uint32_t)-1, 1, (uint32_t)-1, (uint32_t)-1};
    int result = -1;
    SceSysmoduleOpt opt;

    memset(&opt, 0, sizeof(opt));
    opt.flags = sizeof(opt);
    opt.result = &result;
    opt.unused[0] = -1;
    opt.unused[1] = -1;
    return sceSysmoduleLoadModuleInternalWithArg(SCE_SYSMODULE_INTERNAL_PAF, sizeof(argp), argp, &opt);
}

int vpk_promote(const char *path, const char **stage)
{
    SceSysmoduleOpt opt;
    int res;

    *stage = "load PAF module";
    res = paf_load();
    if (res < 0) {
        return res;
    }
    *stage = "load promoter module";
    res = sceSysmoduleLoadModuleInternal(SCE_SYSMODULE_INTERNAL_PROMOTER_UTIL);
    if (res >= 0) {
        *stage = "promoter init";
        res = scePromoterUtilityInit();
        if (res >= 0) {
            *stage = "promote package";
            res = scePromoterUtilityPromotePkgWithRif(path, 1);
            scePromoterUtilityExit();
        }
        sceSysmoduleUnloadModuleInternal(SCE_SYSMODULE_INTERNAL_PROMOTER_UTIL);
    }
    memset(&opt, 0, sizeof(opt));
    sceSysmoduleUnloadModuleInternalWithArg(SCE_SYSMODULE_INTERNAL_PAF, 0, NULL, &opt);
    return res;
}
