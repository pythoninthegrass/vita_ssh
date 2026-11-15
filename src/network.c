#include "network.h"

#include <psp2/kernel/processmgr.h>
#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <psp2/sysmodule.h>
#include <string.h>

void *net_memory = NULL;
SceUID net_memid = -1;

int load_network_modules() {
    net_log("Loading network modules...");
    
    int ret = sceSysmoduleIsLoaded(SCE_SYSMODULE_NET);
    if (ret != SCE_SYSMODULE_LOADED) {
        ret = sceSysmoduleLoadModule(SCE_SYSMODULE_NET);
        if (ret < 0) {
            net_log("Error loading SceNet: 0x%08X", ret);
            return -1;
        }
    } else {
        net_log("SceNet already loaded");
    }

    ret = sceSysmoduleIsLoaded(SCE_SYSMODULE_NET_CTL);
    if (ret != SCE_SYSMODULE_LOADED) {
        ret = sceSysmoduleLoadModule(SCE_SYSMODULE_NET_CTL);
        if (ret < 0 && ret != 0x805A1000) { // Ignore error if loaded
            net_log("Error loading SceNetCtl: 0x%08X", ret);
            sceSysmoduleUnloadModule(SCE_SYSMODULE_NET);
            return -1;
        }
    } else {
        net_log("SceNetCtl already loaded");
    }
    
    return 0;
}

int init_network() {
    net_memid = sceKernelAllocMemBlock("NetMem", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, 512 * 1024, NULL);
    if (net_memid < 0) {
        net_log("Error reserving memory block: 0x%08X", net_memid);
        return -1;
    }

    sceKernelGetMemBlockBase(net_memid, &net_memory);
    if (!net_memory) {
        net_log("Error getting memory base");
        sceKernelFreeMemBlock(net_memid);
        return -1;
    }

    SceNetInitParam param;
    memset(&param, 0, sizeof(param));
    param.memory = net_memory;
    param.size = 512 * 1024;
    param.flags = 0;

    net_log("Initializing sceNet...");
    int ret = sceNetInit(&param);
    if (ret < 0) {
        net_log("Error starting sceNet: 0x%08X", ret);
        sceKernelFreeMemBlock(net_memid);
        return ret;
    }

    net_log("Initializing sceNetCtl...");
    ret = sceNetCtlInit();
    if (ret < 0) {
        net_log("Error starting sceNetCtl: 0x%08X", ret);
        sceKernelFreeMemBlock(net_memid);
        return ret;
    }

    net_log("Waiting for network connection...");
    int state = 0;
    int timeout = 10;
    while (timeout--) {
        ret = sceNetCtlInetGetState(&state);
        if (ret == 0 && state == SCE_NETCTL_STATE_CONNECTED) {
            net_log("Network ready and connected");
            return 0;
        }
        net_log("Network state: %d, ret: 0x%08X", state, ret);
        sceKernelDelayThread(1000 * 1000);
    }

    net_log("Could not connect to network: state=%d", state);
    sceKernelFreeMemBlock(net_memid);
    return -1;
}

void cleanup_network() {
    if (net_memid >= 0) {
        sceKernelFreeMemBlock(net_memid);
        net_memid = -1;
    }
}
