/* SPDX-License-Identifier: GPL-2.0-or-later */
#define _GNU_SOURCE
#include "dbc_rw.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static pid_t g_bound_pid;

/* ARM64 直接 svc，避免 libc socket 包装干扰 */
static long dbc_rw_svc5(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3,
                        uint64_t a4)
{
    register long x8 __asm__("x8") = DBC_RW_SYSCALL_NR;
    register long x0 __asm__("x0") = (long)a0;
    register long x1 __asm__("x1") = (long)a1;
    register long x2 __asm__("x2") = (long)a2;
    register long x3 __asm__("x3") = (long)a3;
    register long x4 __asm__("x4") = (long)a4;
    __asm__ __volatile__("svc 0"
                         : "+r"(x0)
                         : "r"(x8), "r"(x1), "r"(x2), "r"(x3), "r"(x4)
                         : "memory", "cc");
    return x0;
}

long dbc_rw_xcall(unsigned cmd, uint64_t a1, uint64_t a2, uint64_t a3,
                  uint64_t a4)
{
    return dbc_rw_svc5(DBC_RW_MAGIC | (cmd & 0xFFu), a1, a2, a3, a4);
}

long dbc_rw_version(void)
{
    return dbc_rw_xcall(DBC_RW_CMD_VERSION, 0, 0, 0, 0);
}

int dbc_rw_available(void)
{
    return dbc_rw_version() > 0;
}

int dbc_rw_bind(pid_t pid)
{
    if (pid <= 0)
        return -1;
    if (!dbc_rw_available())
        return -1;
    g_bound_pid = pid;
    return 0;
}

pid_t dbc_rw_bound_pid(void)
{
    return g_bound_pid;
}

ssize_t dbc_rw_read_pid(pid_t pid, uint64_t addr, void *buf, size_t size)
{
    long r;
    if (pid <= 0 || !buf || size == 0)
        return -1;
    r = dbc_rw_xcall(DBC_RW_CMD_READ, (uint64_t)(unsigned)pid, addr,
                     (uint64_t)(uintptr_t)buf, (uint64_t)size);
    if (r == (long)size)
        return (ssize_t)size;
    return -1;
}

ssize_t dbc_rw_write_pid(pid_t pid, uint64_t addr, const void *buf, size_t size)
{
    long r;
    if (pid <= 0 || !buf || size == 0)
        return -1;
    r = dbc_rw_xcall(DBC_RW_CMD_WRITE, (uint64_t)(unsigned)pid, addr,
                     (uint64_t)(uintptr_t)buf, (uint64_t)size);
    if (r == (long)size)
        return (ssize_t)size;
    return -1;
}

ssize_t dbc_rw_read(uint64_t addr, void *buf, size_t size)
{
    return dbc_rw_read_pid(g_bound_pid, addr, buf, size);
}

ssize_t dbc_rw_write(uint64_t addr, const void *buf, size_t size)
{
    return dbc_rw_write_pid(g_bound_pid, addr, buf, size);
}

#define DEF_R(name, T)                                                         \
    T dbc_rw_##name(uint64_t addr)                                             \
    {                                                                          \
        T v;                                                                   \
        memset(&v, 0, sizeof(v));                                              \
        if (dbc_rw_read(addr, &v, sizeof(v)) != (ssize_t)sizeof(v))            \
            return (T)0;                                                       \
        return v;                                                              \
    }

#define DEF_W(name, T)                                                         \
    int dbc_rw_##name(uint64_t addr, T v)                                      \
    {                                                                          \
        return dbc_rw_write(addr, &v, sizeof(v)) == (ssize_t)sizeof(v);        \
    }

uint8_t dbc_rw_r8(uint64_t addr)
{
    uint8_t v = 0;
    if (dbc_rw_read(addr, &v, 1) != 1)
        return 0;
    return v;
}
uint16_t dbc_rw_r16(uint64_t addr)
{
    uint16_t v = 0;
    if (dbc_rw_read(addr, &v, 2) != 2)
        return 0;
    return v;
}
uint32_t dbc_rw_r32(uint64_t addr)
{
    uint32_t v = 0;
    if (dbc_rw_read(addr, &v, 4) != 4)
        return 0;
    return v;
}
uint64_t dbc_rw_r64(uint64_t addr)
{
    uint64_t v = 0;
    if (dbc_rw_read(addr, &v, 8) != 8)
        return 0;
    return v;
}
float dbc_rw_rf(uint64_t addr)
{
    float v = 0;
    if (dbc_rw_read(addr, &v, sizeof(v)) != (ssize_t)sizeof(v))
        return 0;
    return v;
}
double dbc_rw_rd(uint64_t addr)
{
    double v = 0;
    if (dbc_rw_read(addr, &v, sizeof(v)) != (ssize_t)sizeof(v))
        return 0;
    return v;
}

int dbc_rw_w8(uint64_t addr, uint8_t v)
{
    return dbc_rw_write(addr, &v, 1) == 1;
}
int dbc_rw_w16(uint64_t addr, uint16_t v)
{
    return dbc_rw_write(addr, &v, 2) == 2;
}
int dbc_rw_w32(uint64_t addr, uint32_t v)
{
    return dbc_rw_write(addr, &v, 4) == 4;
}
int dbc_rw_w64(uint64_t addr, uint64_t v)
{
    return dbc_rw_write(addr, &v, 8) == 8;
}
int dbc_rw_wf(uint64_t addr, float v)
{
    return dbc_rw_write(addr, &v, sizeof(v)) == (ssize_t)sizeof(v);
}
int dbc_rw_wd(uint64_t addr, double v)
{
    return dbc_rw_write(addr, &v, sizeof(v)) == (ssize_t)sizeof(v);
}

uint64_t dbc_rw_module_base(pid_t pid, const char *name_substr)
{
    char path[64], line[512];
    FILE *fp;
    uint64_t start = 0;

    if (pid <= 0 || !name_substr || !name_substr[0])
        return 0;
    snprintf(path, sizeof(path), "/proc/%d/maps", (int)pid);
    fp = fopen(path, "r");
    if (!fp)
        return 0;
    while (fgets(line, sizeof(line), fp)) {
        if (!strstr(line, name_substr))
            continue;
        if (sscanf(line, "%lx", &start) == 1)
            break;
        start = 0;
    }
    fclose(fp);
    return start;
}

pid_t dbc_rw_pidof(const char *package)
{
    char cmd[256];
    FILE *fp;
    int pid = 0;

    if (!package || !package[0])
        return 0;
    snprintf(cmd, sizeof(cmd), "pidof %s 2>/dev/null", package);
    fp = popen(cmd, "r");
    if (!fp)
        return 0;
    if (fscanf(fp, "%d", &pid) != 1)
        pid = 0;
    pclose(fp);
    return (pid_t)pid;
}
