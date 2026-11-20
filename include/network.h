#ifndef NETWORK_H
#define NETWORK_H

#include <psp2/kernel/sysmem.h>

#ifndef SCE_SYSMODULE_NET_CTL
#define SCE_SYSMODULE_NET_CTL 0x9
#endif

int init_network();
/* init_network() in two halves, for a caller that waits for Wi-Fi its own way. */
int init_network_start();
int network_connected();
void network_hold_wireless();
void cleanup_network();
int load_network_modules();

/*
 * One line of bring-up logging, implemented by each app: network.c is compiled
 * into both, and the server has no debug screen to printf() to. The format
 * string carries no trailing newline — the implementation supplies its own line
 * terminator, so a caller reads as one message, not a message and a blank line.
 */
void net_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

extern void *net_memory;
extern SceUID net_memid;

#endif
