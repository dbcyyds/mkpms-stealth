#ifndef KLOAD_API_H
#define KLOAD_API_H

#define PR_KLOAD_PING     0x4B4C0001
/* prctl(opt, pid, path_uaddr_in_target, dlopen_va, 0) */
#define PR_KLOAD_INJECT   0x4B4C0010
#define PR_KLOAD_STATUS   0x4B4C0011
#define PR_KLOAD_CLEAR    0x4B4C0012
#define KLOAD_MAGIC       0x4B4C

#endif
