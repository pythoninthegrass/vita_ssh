/*
 * Builds sce_sys/package/head.bin for scePromoterUtilityPromotePkgWithRif from the template
 * in third_party/vitashell/head.bin. Pure C, no sce* calls, so it also builds on the host for
 * scripts/test_vpk_head.py.
 */
#ifndef VPK_HEAD_H
#define VPK_HEAD_H

#include <stddef.h>

#define VPK_CONTENT_ID_MAX 48

/* Copies tmpl to out (tmpl_len bytes), writes content_id at offset 0x30 and patches the three
 * MACs the header carries. Returns 0, or -1 when a header offset points outside the template. */
int vpk_make_head_bin(const unsigned char *tmpl, size_t tmpl_len, const char *content_id,
                      unsigned char *out);

#endif
