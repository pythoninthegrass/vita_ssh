/* What `vpk install` may do with a package; see include/vpk_policy.h. */
#include "vpk_policy.h"

#include <string.h>

#define TITLE_LEN 9

int vpk_title_ok(const char *title)
{
    int i;

    if (title == NULL || strlen(title) != TITLE_LEN || title[0] < 'A' || title[0] > 'Z') {
        return 0;
    }
    for (i = 1; i < TITLE_LEN; i++) {
        if (!((title[i] >= 'A' && title[i] <= 'Z') || (title[i] >= '0' && title[i] <= '9'))) {
            return 0;
        }
    }
    return 1;
}

int vpk_title_is_system(const char *title)
{
    return title != NULL && strncmp(title, "NPXS", 4) == 0;
}

vpk_verdict_t vpk_decide(const char *title, const char *own, const char *updater, int installed, int running,
                         int replace)
{
    if (!vpk_title_ok(title)) {
        return VPK_BAD_TITLE;
    }
    if (own != NULL && strcmp(title, own) == 0) {
        return VPK_OWN;
    }
    if (updater != NULL && strcmp(title, updater) == 0) {
        return VPK_REFUSE_UPDATER;
    }
    if (vpk_title_is_system(title)) {
        return VPK_REFUSE_SYSTEM;
    }
    if (running) {
        return VPK_REFUSE_RUNNING;
    }
    if (installed) {
        return replace ? VPK_REPLACE : VPK_REFUSE_INSTALLED;
    }
    return VPK_INSTALL;
}

const char *vpk_verdict_name(vpk_verdict_t v)
{
    switch (v) {
    case VPK_OWN:
        return "own";
    case VPK_INSTALL:
        return "install";
    case VPK_REPLACE:
        return "replace";
    case VPK_BAD_TITLE:
        return "bad-title";
    case VPK_REFUSE_UPDATER:
        return "updater";
    case VPK_REFUSE_SYSTEM:
        return "system";
    case VPK_REFUSE_RUNNING:
        return "running";
    case VPK_REFUSE_INSTALLED:
        return "installed";
    }
    return "unknown";
}
