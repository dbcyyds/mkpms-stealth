/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * dbc_rw 使用示例
 *
 * 编译（设备 aarch64）：
 *   aarch64-linux-gnu-gcc -O2 -static -o example_rw \
 *       example_rw.c dbc_rw.c
 *
 * 运行前 load KPM（二选一）：
 *   kpatch KEY kpm load /data/local/tmp/stealth.kpm   # 推荐，已含 dbc-rw
 *   kpatch KEY kpm load /data/local/tmp/dbc-rw.kpm    # 单独
 *
 * 示例：
 *   ./example_rw                         # 本进程自测读写
 *   ./example_rw -p com.tencent.letsgo -m libUE4.so -o 0
 *   ./example_rw -p 12345 -a 0x7b00000000
 */
#define _GNU_SOURCE
#include "dbc_rw.h"

#include <getopt.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void usage(const char *argv0)
{
    fprintf(stderr,
            "Usage:\n"
            "  %s                         self read/write demo\n"
            "  %s -p <pkg|pid> -m <so> [-o off]\n"
            "  %s -p <pkg|pid> -a <hex_addr>\n"
            "\n"
            "  -p  package name or numeric pid\n"
            "  -m  maps substring (module base)\n"
            "  -o  offset from module base (hex/dec)\n"
            "  -a  absolute address (hex)\n"
            "  -w  write uint64 value (hex) then re-read\n",
            argv0, argv0, argv0);
}

static pid_t parse_pid_or_pkg(const char *s)
{
    char *end = NULL;
    long v;

    if (!s || !s[0])
        return 0;
    v = strtol(s, &end, 10);
    if (end && *end == '\0' && v > 0)
        return (pid_t)v;
    return dbc_rw_pidof(s);
}

/* 本进程：验证内核读写通道 */
static int demo_self(void)
{
    volatile uint64_t slot = 0xCAFEBABE11223344ULL;
    uint64_t got = 0;
    long ver;
    pid_t me = getpid();

    ver = dbc_rw_version();
    printf("[*] dbc_rw_version = 0x%lx\n", (unsigned long)ver);
    if (ver <= 0) {
        fprintf(stderr, "[-] KPM not loaded (need stealth.kpm or dbc-rw.kpm)\n");
        return 1;
    }
    if (dbc_rw_bind(me) != 0) {
        fprintf(stderr, "[-] bind failed\n");
        return 1;
    }

    printf("[*] self pid=%d slot@%p value=0x%llx\n", (int)me, (void *)&slot,
           (unsigned long long)slot);

    if (dbc_rw_read((uint64_t)(uintptr_t)&slot, &got, 8) != 8) {
        fprintf(stderr, "[-] read failed\n");
        return 2;
    }
    printf("[+] read  -> 0x%llx %s\n", (unsigned long long)got,
           got == slot ? "OK" : "MISMATCH");

    if (!dbc_rw_w64((uint64_t)(uintptr_t)&slot, 0x8899AABBCCDDEEFFULL)) {
        fprintf(stderr, "[-] write failed\n");
        return 3;
    }
    printf("[+] write -> 0x8899AABBCCDDEEFF\n");
    printf("[+] direct load after write: 0x%llx\n",
           (unsigned long long)slot);

    got = dbc_rw_r64((uint64_t)(uintptr_t)&slot);
    printf("[+] r64   -> 0x%llx %s\n", (unsigned long long)got,
           got == 0x8899AABBCCDDEEFFULL ? "OK" : "MISMATCH");

    /* 恢复 */
    slot = 0xCAFEBABE11223344ULL;
    printf("[*] self demo done\n");
    return 0;
}

static int demo_remote(pid_t pid, uint64_t addr, int do_write, uint64_t wval)
{
    uint64_t got = 0;
    long ver = dbc_rw_version();

    printf("[*] version=0x%lx pid=%d addr=0x%" PRIx64 "\n",
           (unsigned long)ver, (int)pid, addr);
    if (ver <= 0) {
        fprintf(stderr, "[-] KPM not loaded\n");
        return 1;
    }
    if (dbc_rw_bind(pid) != 0) {
        fprintf(stderr, "[-] bind failed\n");
        return 1;
    }

    if (dbc_rw_read(addr, &got, 8) != 8) {
        fprintf(stderr, "[-] read 8 failed (page not present / bad addr?)\n");
        return 2;
    }
    printf("[+] r64 = 0x%016" PRIx64 "\n", got);

    if (do_write) {
        if (!dbc_rw_w64(addr, wval)) {
            fprintf(stderr, "[-] write failed\n");
            return 3;
        }
        got = dbc_rw_r64(addr);
        printf("[+] after write r64 = 0x%016" PRIx64 "\n", got);
    }
    return 0;
}

int main(int argc, char **argv)
{
    const char *pkg = NULL;
    const char *mod = NULL;
    uint64_t addr = 0, off = 0, wval = 0;
    int do_write = 0;
    int opt;
    pid_t pid;

    while ((opt = getopt(argc, argv, "p:m:o:a:w:h")) != -1) {
        switch (opt) {
        case 'p':
            pkg = optarg;
            break;
        case 'm':
            mod = optarg;
            break;
        case 'o':
            off = strtoull(optarg, NULL, 0);
            break;
        case 'a':
            addr = strtoull(optarg, NULL, 0);
            break;
        case 'w':
            do_write = 1;
            wval = strtoull(optarg, NULL, 0);
            break;
        case 'h':
        default:
            usage(argv[0]);
            return opt == 'h' ? 0 : 1;
        }
    }

    if (!pkg && !addr)
        return demo_self();

    if (!pkg) {
        usage(argv[0]);
        return 1;
    }
    pid = parse_pid_or_pkg(pkg);
    if (pid <= 0) {
        fprintf(stderr, "[-] cannot resolve pid for '%s'\n", pkg);
        return 1;
    }

    if (!addr) {
        if (!mod) {
            fprintf(stderr, "[-] need -m module or -a addr\n");
            return 1;
        }
        addr = dbc_rw_module_base(pid, mod);
        if (!addr) {
            fprintf(stderr, "[-] module base not found for '%s' (maps hidden?)\n",
                    mod);
            return 1;
        }
        addr += off;
        printf("[*] base('%s')+0x%" PRIx64 " => 0x%" PRIx64 "\n", mod, off,
               addr);
    }

    return demo_remote(pid, addr, do_write, wval);
}
