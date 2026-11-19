/* Size arithmetic for the `du` builtin: no I/O and no Vita APIs. */
#ifndef SH_DU_H
#define SH_DU_H

#include <stddef.h>

/* A file's size in 1K units: st_size rounded up, so one byte counts 1 and an empty file 0. */
long long sh_du_kb(long long bytes);

/* GNU `du -h` text for a size given in 1K units: K, M, G or T, rounded up, with one decimal
 * under 10 ("4.0K", "2.3M") and none from 10 up ("254M"). Zero is "0". */
void sh_du_human(char *dst, size_t dstlen, long long kb);

#endif
