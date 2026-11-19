#include "sh_du.h"

#include <stdio.h>

long long sh_du_kb(long long bytes)
{
    if (bytes <= 0) {
        return 0;
    }
    return bytes / 1024 + (bytes % 1024 != 0);
}

void sh_du_human(char *dst, size_t dstlen, long long kb)
{
    static const char units[] = "KMGT";
    long long unit = 1, tenths, whole;
    int u = 0;

    if (kb <= 0) {
        snprintf(dst, dstlen, "0");
        return;
    }
    while (kb >= unit * 1024 && u < 3) {
        unit *= 1024;
        u++;
    }
    tenths = (kb * 10 + unit - 1) / unit;
    if (tenths < 100) {
        snprintf(dst, dstlen, "%lld.%lld%c", tenths / 10, tenths % 10, units[u]);
        return;
    }
    whole = (kb + unit - 1) / unit;
    if (whole >= 1024 && u < 3) {
        snprintf(dst, dstlen, "1.0%c", units[u + 1]);
        return;
    }
    snprintf(dst, dstlen, "%lld%c", whole, units[u]);
}
