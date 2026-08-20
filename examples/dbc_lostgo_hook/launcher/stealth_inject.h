/* SPDX-License-Identifier: GPL-2.0-or-later */
/* 与 hide-maps.kpm prctl ABI 保持一致 */
#ifndef STEALTH_INJECT_H
#define STEALTH_INJECT_H

#include <stdint.h>

#define PR_HIDEMAPS_PING       0x484D0001
#define PR_HIDEMAPS_ADD_KW     0x484D0002
#define PR_HIDEMAPS_ADD_RANGE  0x484D0003  /* prctl(opt, start, end, 0, 0) end exclusive */
#define PR_HIDEMAPS_CLR_RANGE  0x484D0004
#define PR_HIDEMAPS_REGISTER   0x484D0010  /* prctl(opt, pkg, so, flags, 0) */
#define PR_HIDEMAPS_QUERY      0x484D0011
#define PR_HIDEMAPS_CLEAR      0x484D0012
#define HIDEMAPS_MAGIC         0x484D

#endif
