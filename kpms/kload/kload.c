/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * kload — 内核无 ptrace 注入 so
 *
 * 原理:
 *   1) 用户态 prctl(KLOAD_INJECT, pid, path, dlopen_addr, 0) 登记任务
 *   2) 目标进程任意 syscall 返回用户态前，改写 pt_regs:
 *        x0 = path (写在目标栈上)
 *        x1 = RTLD_NOW (2)
 *        x2 = 0
 *        lr = 原 pc
 *        pc = dlopen / __loader_dlopen
 *   3) 目标在自身上下文执行 dlopen，完成链接与 constructor
 *
 * 配合 hide-maps 藏 maps；无 ptrace。
 *
 * ABI (与 tools/stealth_inject 共用):
 *   PR_KLOAD_PING    0x4B4C0001  → 0x4B4C
 *   PR_KLOAD_INJECT  0x4B4C0010  arg1=pid, arg2=path_user, arg3=dlopen_va
 *   PR_KLOAD_STATUS  0x4B4C0011  → status (0 none, 1 pending, 2 done, <0 err)
 *   PR_KLOAD_CLEAR   0x4B4C0012
 */

#include <compiler.h>
#include <kpmodule.h>
#include <linux/printk.h>
#include <common.h>
#include <kputils.h>
#include <linux/string.h>
#include <hook.h>
#include <syscall.h>
#include <asm/current.h>
#include <asm/ptrace.h>
#include <uapi/asm-generic/unistd.h>
#include "../common/kpm_demo_helpers.h"

#ifndef STEALTH_FUSED
KPM_MODULE_INFO("kload",
                "1.0.0",
                "GPL v2",
                "dbc",
                "Kernel so inject without ptrace (syscall-exit dlopen hijack)");
#endif

#define PR_KLOAD_PING     0x4B4C0001
#define PR_KLOAD_INJECT   0x4B4C0010
#define PR_KLOAD_STATUS   0x4B4C0011
#define PR_KLOAD_CLEAR    0x4B4C0012
#define KLOAD_MAGIC       0x4B4C

#define RTLD_NOW          2
#define PATH_MAX_K        280
#define GFP_KERNEL_VAL    0xcc0

#ifndef __NR_prctl
#define __NR_prctl 167
#endif
#ifndef __NR_clock_gettime
#define __NR_clock_gettime 113
#endif
#ifndef __NR_gettimeofday
#define __NR_gettimeofday 169
#endif
#ifndef __NR_getpid
#define __NR_getpid 172
#endif
#ifndef __NR_openat
#define __NR_openat 56
#endif
#ifndef __NR_read
#define __NR_read 63
#endif
#ifndef __NR_futex
#define __NR_futex 98
#endif

enum pid_type {
    PIDTYPE_PID,
    PIDTYPE_TGID,
    PIDTYPE_PGID,
    PIDTYPE_SID,
    PIDTYPE_MAX,
};
struct pid_namespace;

extern int has_syscall_wrapper;

static pid_t (*k_task_pid_nr_ns)(struct task_struct *task, enum pid_type type,
                                 struct pid_namespace *ns);

/* 待注入任务 */
static int g_pending;           /* 0 none 1 wait 2 done */
static int g_last_err;
static pid_t g_target_tgid;
static unsigned long g_dlopen;
static unsigned long g_path_uaddr; /* 目标进程内已写好的 path 用户态地址 */
static char g_path[PATH_MAX_K];   /* 仅日志 */
static int g_hooked_prctl;
static int g_hooked_exit;

static pid_t current_tgid(void)
{
    if (k_task_pid_nr_ns)
        return k_task_pid_nr_ns(current, PIDTYPE_TGID, 0);
    return -1;
}

static struct pt_regs *fargs_to_regs(void *fargs)
{
    hook_fargs0_t *fa = (hook_fargs0_t *)fargs;
    if (!fa)
        return NULL;
    if (has_syscall_wrapper)
        return (struct pt_regs *)fa->args[0];
    return NULL;
}

/*
 * 路径由用户态预先写入目标进程 RW 匿名页（/proc/pid/mem），
 * 内核只改寄存器跳 dlopen，避免栈上 path 被覆盖。
 */
static int do_hijack_dlopen(void *fargs)
{
    struct pt_regs *regs;
    hook_fargs0_t *fa;
    unsigned long old_pc;

    if (g_pending != 1 || !g_dlopen || !g_path_uaddr)
        return 0;

    if (current_tgid() != g_target_tgid)
        return 0;

    fa = (hook_fargs0_t *)fargs;
    regs = fargs_to_regs(fargs);
    if (!regs) {
        g_last_err = -5;
        return 0;
    }

    old_pc = regs->pc;
    /*
     * 有 syscall wrapper 时，返回用户态 x0 往往取自 fargs->ret，
     * 只改 regs[0] 会被 syscall 返回值覆盖 → path 变小整数 → 崩在 0x1000。
     */
    fa->ret = g_path_uaddr;
    regs->regs[0] = g_path_uaddr;
    regs->regs[1] = RTLD_NOW;
    regs->regs[2] = 0;
    /* 保留原 lr */
    regs->pc = g_dlopen;

    g_pending = 2;
    g_last_err = 0;
    pr_info("kload: hijack tgid=%d dlopen=%lx path_u=%lx lr=%lx old_pc=%lx\n",
            (int)g_target_tgid, g_dlopen, g_path_uaddr,
            (unsigned long)regs->regs[30], old_pc);
    return 1;
}

/* 高频 syscall 的 after：尝试劫持 */
static void after_any_sc(hook_fargs4_t *args, void *udata)
{
    (void)udata;
    if (g_pending != 1)
        return;
    do_hijack_dlopen(args);
}

static void prctl_before(hook_fargs4_t *args, void *udata)
{
    int option = (int)syscall_argn(args, 0);
    unsigned long a1 = syscall_argn(args, 1);
    unsigned long a2 = syscall_argn(args, 2);
    unsigned long a3 = syscall_argn(args, 3);
    char path[PATH_MAX_K];
    long n;

    (void)udata;

    if (option < PR_KLOAD_PING || option > PR_KLOAD_CLEAR)
        return;

    switch (option) {
    case PR_KLOAD_PING:
        args->ret = KLOAD_MAGIC;
        args->skip_origin = 1;
        break;

    case PR_KLOAD_STATUS:
        args->ret = g_pending;
        if (g_pending == 0 && g_last_err)
            args->ret = g_last_err;
        args->skip_origin = 1;
        break;

    case PR_KLOAD_CLEAR:
        g_pending = 0;
        g_target_tgid = 0;
        g_dlopen = 0;
        g_path_uaddr = 0;
        g_path[0] = '\0';
        g_last_err = 0;
        args->ret = 0;
        args->skip_origin = 1;
        break;

    case PR_KLOAD_INJECT:
        /*
         * prctl(INJECT, pid, path_uaddr, dlopen_va, 0)
         * path_uaddr: 目标进程内已写好的 C 字符串地址（用户态 /proc/pid/mem 写入）
         * 为兼容旧工具：若 a2 是可拷贝的用户指针且内容以'/'开头，则当作路径串
         * 新协议：a2 = 远程 path 地址（高地址 > 0x10000 且进程内）
         *
         * 判定：a2 在工具进程里若 strncpy 得到 '/' 开头路径 → 旧协议（仅日志，仍要求远程）
         * 实际：用户态只传远程 path_uaddr（数值地址），内核不再 copy 路径串。
         */
        if (!a1 || !a2 || !a3) {
            args->ret = -22;
            args->skip_origin = 1;
            break;
        }
        /* 可选：若 a2 指向本进程字符串则复制到 g_path 作日志 */
        n = compat_strncpy_from_user(path, (const char __user *)a2, sizeof(path));
        if (n > 0 && path[0] == '/') {
            /* 旧协议：a2 是调用者进程内的路径串 — 不支持跨进程，拒绝 */
            pr_err("kload: pass remote path uaddr, not local string\n");
            args->ret = -22;
            args->skip_origin = 1;
            break;
        }

        g_target_tgid = (pid_t)a1;
        g_path_uaddr = a2;
        g_dlopen = a3;
        snprintf(g_path, sizeof(g_path), "u:%lx", g_path_uaddr);
        g_pending = 1;
        g_last_err = 0;
        pr_info("kload: INJECT armed tgid=%d dlopen=%lx path_u=%lx\n",
                (int)g_target_tgid, g_dlopen, g_path_uaddr);
        args->ret = 0;
        args->skip_origin = 1;
        break;

    default:
        break;
    }
}

static int install_hooks(void)
{
    int n = 0;
    /* prctl API */
    if (hook_syscalln(__NR_prctl, 5, prctl_before, NULL, NULL) == 0) {
        g_hooked_prctl = 1;
        n++;
        pr_info("kload: hooked prctl\n");
    } else {
        pr_err("kload: prctl hook fail\n");
        return -1;
    }

    /*
     * 多挂几个常见 syscall 的 after，尽快命中目标进程。
     * 不 hook 全部 syscall，降低开销。
     */
    if (hook_syscalln(__NR_clock_gettime, 2, NULL, after_any_sc, NULL) == 0)
        n++;
    if (hook_syscalln(__NR_gettimeofday, 2, NULL, after_any_sc, NULL) == 0)
        n++;
    if (hook_syscalln(__NR_getpid, 0, NULL, after_any_sc, NULL) == 0)
        n++;
    if (hook_syscalln(__NR_openat, 4, NULL, after_any_sc, NULL) == 0)
        n++;
    if (hook_syscalln(__NR_read, 3, NULL, after_any_sc, NULL) == 0)
        n++;
    if (hook_syscalln(__NR_futex, 6, NULL, after_any_sc, NULL) == 0)
        n++;

    g_hooked_exit = 1;
    pr_info("kload: syscall after-hooks installed (n=%d wrapper=%d)\n",
            n, has_syscall_wrapper);
    return 0;
}

static void remove_hooks(void)
{
    if (g_hooked_prctl) {
        unhook_syscalln(__NR_prctl, prctl_before, NULL);
        g_hooked_prctl = 0;
    }
    if (g_hooked_exit) {
        unhook_syscalln(__NR_clock_gettime, NULL, after_any_sc);
        unhook_syscalln(__NR_gettimeofday, NULL, after_any_sc);
        unhook_syscalln(__NR_getpid, NULL, after_any_sc);
        unhook_syscalln(__NR_openat, NULL, after_any_sc);
        unhook_syscalln(__NR_read, NULL, after_any_sc);
        unhook_syscalln(__NR_futex, NULL, after_any_sc);
        g_hooked_exit = 0;
    }
}

long kload_init(const char *args, const char *event, void *__user reserved)
{
    (void)reserved;
    kpm_demo_log_init("kload", event, args);

    k_task_pid_nr_ns = (void *)kallsyms_lookup_name("__task_pid_nr_ns");
    if (!k_task_pid_nr_ns)
        pr_warn("kload: __task_pid_nr_ns missing\n");

    g_pending = 0;
    g_path[0] = '\0';

    if (install_hooks() < 0)
        return -1;

    pr_info("kload: ready prctl PING=0x%x INJECT=0x%x\n",
            PR_KLOAD_PING, PR_KLOAD_INJECT);
    return 0;
}

long kload_control0(const char *args, char *__user out_msg, int outlen)
{
    char buf[192];
    snprintf(buf, sizeof(buf), "pending=%d tgid=%d dlopen=%lx err=%d path=%s",
             g_pending, (int)g_target_tgid, g_dlopen, g_last_err, g_path);
    (void)args;
    return kpm_demo_copy_message(buf, out_msg, outlen);
}

long kload_exit(void *__user reserved)
{
    (void)reserved;
    remove_hooks();
    return kpm_demo_log_exit("kload");
}

#ifndef STEALTH_FUSED
KPM_INIT(kload_init);
KPM_CTL0(kload_control0);
KPM_EXIT(kload_exit);
#endif
