/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * dbc_rw — 用户态封装：内核跨进程读写（syscall 41 + magic）
 *
 * 依赖 KPM：stealth.kpm（含 dbc-rw）或单独 dbc-rw.kpm / dbc
 *
 * 协议：
 *   long ret = syscall(41, MAGIC|cmd, a1, a2, a3, a4);
 *   cmd 0 VERSION → ret = 0x20260725
 *   cmd 1 READ    → a1=pid a2=addr a3=buf a4=size → ret=size|0
 *   cmd 2 WRITE   → 同上
 */
#ifndef DBC_RW_H
#define DBC_RW_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DBC_RW_MAGIC           0x1b1841fd2c1e0000ULL
#define DBC_RW_CMD_VERSION     0
#define DBC_RW_CMD_READ        1
#define DBC_RW_CMD_WRITE       2
#define DBC_RW_VERSION_OK      0x20260725
#define DBC_RW_SYSCALL_NR      41  /* socket，被 KPM hook */

/* 底层：一次 syscall，返回内核 ret */
long dbc_rw_xcall(unsigned cmd, uint64_t a1, uint64_t a2, uint64_t a3,
                  uint64_t a4);

/* 探测模块；成功返回版本号（>=0x20260725 新版），失败 <0 或 0 */
long dbc_rw_version(void);

/* 是否已 load（version > 0） */
int dbc_rw_available(void);

/*
 * 绑定默认目标 pid（后续 read/write 可省略 pid 的便捷 API）
 * 返回 0 成功（模块在且 pid>0）
 */
int dbc_rw_bind(pid_t pid);
pid_t dbc_rw_bound_pid(void);

/* 显式 pid 读写：成功返回 size，失败返回 -1 */
ssize_t dbc_rw_read_pid(pid_t pid, uint64_t addr, void *buf, size_t size);
ssize_t dbc_rw_write_pid(pid_t pid, uint64_t addr, const void *buf, size_t size);

/* 使用 bind 过的 pid */
ssize_t dbc_rw_read(uint64_t addr, void *buf, size_t size);
ssize_t dbc_rw_write(uint64_t addr, const void *buf, size_t size);

/* 类型化便捷（失败返回 0 / false） */
uint8_t  dbc_rw_r8(uint64_t addr);
uint16_t dbc_rw_r16(uint64_t addr);
uint32_t dbc_rw_r32(uint64_t addr);
uint64_t dbc_rw_r64(uint64_t addr);
float    dbc_rw_rf(uint64_t addr);
double   dbc_rw_rd(uint64_t addr);

int dbc_rw_w8(uint64_t addr, uint8_t v);
int dbc_rw_w16(uint64_t addr, uint16_t v);
int dbc_rw_w32(uint64_t addr, uint32_t v);
int dbc_rw_w64(uint64_t addr, uint64_t v);
int dbc_rw_wf(uint64_t addr, float v);
int dbc_rw_wd(uint64_t addr, double v);

/* 从 /proc/pid/maps 取模块基址（需可读 maps；hide-maps 可能滤路径） */
uint64_t dbc_rw_module_base(pid_t pid, const char *name_substr);

/* pidof 包名（简单实现） */
pid_t dbc_rw_pidof(const char *package);

#ifdef __cplusplus
}
#endif

#endif /* DBC_RW_H */
