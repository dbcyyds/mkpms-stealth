/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * dbc-rw — 内核跨进程读写（syscall 41 + magic）
 *
 * 用户态：syscall(41, MAGIC|cmd, pid, addr, buf, size)
 * 可单独 load dbc-rw.kpm，或 STEALTH_FUSED 并入 stealth.kpm
 */
#include <common.h>
#include <ktypes.h>
#include <ksyms.h>
#include <compiler.h>
#include <kpmodule.h>
#include <linux/printk.h>
#include <linux/string.h>
#include <syscall.h>
#include <hook.h>
#include "../common/kpm_demo_helpers.h"

#define X_MAGIC 0x1b1841fd2c1e0000ULL
#define X_GET_VERSION 0
#define X_READ_MEMORY 1
#define X_WRITE_MEMORY 2
/* 进程缓存版：同 pid 复用 mm/pgd；pid 变化时切换 */
#define X_VERSION_RET 0x20260725

#ifndef STEALTH_FUSED
KPM_MODULE_INFO("dbc",
                "20260725",
                "GPL v2",
                "dbc",
                "kernel cross-process r/w via syscall(41)+magic, pid cache");
#endif

struct mm_struct;
struct task_struct;
typedef struct { pteval_t pte; } pte_t;

static uint64_t kvar_def(memstart_addr);
static uint64_t kvar_def(kimage_voffset);
static uint64_t my_pa_bits = 48;
static uint64_t my_va_bits = 39;
static uint64_t my_page_shift = 12;

static uintptr_t pgd_offset = 0;
static uint32_t g_mm_offset = 0;

#define GFP_KERNEL 0xD0
#define PHYS_OFFSET kvar_val(memstart_addr)
#define kimage_voffset kvar_val(kimage_voffset)
#define PA_BITS my_pa_bits
#define VA_BITS my_va_bits
#define PAGE_SHIFT my_page_shift
#define PAGE_OFFSET (-(1UL << VA_BITS))
#define PAGE_SIZE (1UL << PAGE_SHIFT)
#define PAGE_LEVEL ((VA_BITS - 4) / (PAGE_SHIFT - 3))
#define PAGE_MASK (~(PAGE_SIZE - 1))

#define pte_val(x) ((x).pte)
#define PTE_ADDR_LOW (((1UL << (48 - PAGE_SHIFT)) - 1) << PAGE_SHIFT)
#define PTE_ADDR_HIGH (0xFUL << 12)

static inline phys_addr_t __pte_to_phys(pte_t pte) {
    return PA_BITS == 52 ?
        (phys_addr_t)(((pte_val(pte) & PTE_ADDR_LOW) + ((pte_val(pte) & PTE_ADDR_HIGH) << 36))) :
        (phys_addr_t)(pte_val(pte) & PTE_ADDR_LOW);
}

#define __phys_to_virt(x) ((unsigned long)((x) - PHYS_OFFSET + PAGE_OFFSET))
#define __virt_to_phys(x) (((phys_addr_t)(x) - PAGE_OFFSET + PHYS_OFFSET))
#define __phys_to_kimg(x) ((unsigned long)((x) + kimage_voffset))

#ifndef min
#define min(x, y) ({ typeof(x) _min1 = (x); typeof(y) _min2 = (y); (void)(&_min1 == &_min2); _min1 < _min2 ? _min1 : _min2; })
#endif

/* ---------- 进程缓存：用户层 pid 不变则复用 ---------- */
struct proc_cache {
    pid_t pid;                 /* 0 = 空 */
    struct task_struct *task;  /* 仅作快速校验，不持引用 */
    struct mm_struct *mm;      /* get_task_mm 持有，切换/退出时 mmput */
    uintptr_t pgd;             /* 用户页表根（内核 VA） */
    /* 同进程内最近一页 VA→PA，避免同页连读反复 walk（非块读 API） */
    uintptr_t page_va;         /* 对齐后的 VA，0 = 无效 */
    phys_addr_t page_pa;       /* 页物理基址 */
};

static struct proc_cache g_pc;

static struct task_struct *find_task_by_vpid(pid_t nr);
static unsigned long copy_from_kernel_nofault(void *dst, const void *src, size_t size);
static uint64_t *walk_pte_entry(uint64_t pgd, uint64_t va);

unsigned long kfunc_def(__arch_copy_from_user)(void *to, const void __user *from, unsigned long n);
static inline unsigned long __arch_copy_from_user(void *to, const void __user *from, unsigned long n) {
    kfunc_direct_call(__arch_copy_from_user, to, from, n);
}

unsigned long kfunc_def(__arch_copy_to_user)(void __user *to, const void *from, unsigned long n);
static inline unsigned long __arch_copy_to_user(void __user *to, const void *from, unsigned long n) {
    kfunc_direct_call(__arch_copy_to_user, to, from, n);
}

unsigned long kfunc_def(copy_from_kernel_nofault)(void *dst, const void *src, size_t size);
static inline unsigned long copy_from_kernel_nofault(void *dst, const void *src, size_t size) {
    kfunc_direct_call(copy_from_kernel_nofault, dst, src, size);
}

unsigned long kfunc_def(copy_to_kernel_nofault)(void *dst, const void *src, size_t size);
static inline unsigned long copy_to_kernel_nofault(void *dst, const void *src, size_t size) {
    kfunc_direct_call(copy_to_kernel_nofault, dst, src, size);
}

struct task_struct *kfunc_def(find_task_by_vpid)(pid_t nr);
static inline struct task_struct *find_task_by_vpid(pid_t nr) {
    kfunc_direct_call(find_task_by_vpid, nr);
}

struct mm_struct *kfunc_def(get_task_mm)(struct task_struct *task);
static inline struct mm_struct *get_task_mm(struct task_struct *task) {
    kfunc_direct_call(get_task_mm, task);
}

void kfunc_def(mmput)(struct mm_struct *mm);
static inline void mmput(struct mm_struct *mm) {
    kfunc_call_void(mmput, mm);
}

int kfunc_def(pfn_valid)(unsigned long pfn);
static inline int pfn_valid(unsigned long pfn) {
    kfunc_direct_call(pfn_valid, pfn);
}

/* memset: use linux/string.h (included via kpm_demo_helpers) */

static void proc_cache_clear(void)
{
    if (g_pc.mm) {
        mmput(g_pc.mm);
        g_pc.mm = NULL;
    }
    g_pc.pid = 0;
    g_pc.task = NULL;
    g_pc.pgd = 0;
    g_pc.page_va = 0;
    g_pc.page_pa = 0;
}

/*
 * 解析并缓存目标进程。
 *  - 用户层 pid 不变 → 直接复用 mm/pgd（不 find_task、不读 task/mm）
 *  - 用户层 pid 变化 → 释放旧缓存，绑定新进程
 * 进程已死但 pid 未改：依赖后续 walk/拷贝失败；调用方可换 pid 或重载模块清缓存。
 */
static int proc_cache_bind(pid_t pid)
{
    struct task_struct *task;
    struct mm_struct *mm;
    struct mm_struct *mm_field = NULL;
    uintptr_t pgd_base = 0;

    if (pid <= 0 || g_mm_offset == 0 || pgd_offset == 0)
        return -1;

    /* 热路径：同 pid 且已有 pgd → 零查找 */
    if (g_pc.pid == pid && g_pc.pgd)
        return 0;

    /* 换进程或首次：丢掉旧缓存 */
    if (g_pc.pid != 0)
        proc_cache_clear();

    task = find_task_by_vpid(pid);
    if (!task)
        return -1;

    /* 优先 get_task_mm 持引用，降低进程退出后 UAF 风险 */
    mm = get_task_mm(task);
    if (!mm) {
        if (copy_from_kernel_nofault(&mm_field,
                                    (void *)((uintptr_t)task + g_mm_offset),
                                    sizeof(mm_field)) != 0 || !mm_field)
            return -1;
        mm = mm_field;
        if (copy_from_kernel_nofault(&pgd_base,
                                    (void *)((uintptr_t)mm + pgd_offset), 8) != 0 ||
            !pgd_base)
            return -1;
        g_pc.pid = pid;
        g_pc.task = task;
        g_pc.mm = NULL; /* 无 mmget，不 mmput */
        g_pc.pgd = pgd_base;
        g_pc.page_va = 0;
        g_pc.page_pa = 0;
        return 0;
    }

    if (copy_from_kernel_nofault(&pgd_base,
                                (void *)((uintptr_t)mm + pgd_offset), 8) != 0 ||
        !pgd_base) {
        mmput(mm);
        return -1;
    }

    g_pc.pid = pid;
    g_pc.task = task;
    g_pc.mm = mm;
    g_pc.pgd = pgd_base;
    g_pc.page_va = 0;
    g_pc.page_pa = 0;
    return 0;
}

static phys_addr_t virt_to_phys_cached(pid_t pid, uintptr_t addr)
{
    uintptr_t page_va;
    uint64_t *pte_va;
    pteval_t pte_val = 0;
    phys_addr_t page_pa;

    if (proc_cache_bind(pid) != 0)
        return 0;

    page_va = addr & PAGE_MASK;

    /* 同页命中：跳过页表 walk */
    if (g_pc.page_va == page_va && g_pc.page_pa)
        return g_pc.page_pa + (addr & (PAGE_SIZE - 1));

    pte_va = walk_pte_entry(g_pc.pgd, addr);
    if (!pte_va)
        return 0;

    if (copy_from_kernel_nofault(&pte_val, pte_va, 8) != 0 || !(pte_val & 1))
        return 0;

    page_pa = (PA_BITS == 52) ?
        (((pte_val & PTE_ADDR_LOW) + ((pte_val & PTE_ADDR_HIGH) << 36))) :
        (pte_val & PTE_ADDR_LOW);
    page_pa &= PAGE_MASK;

    g_pc.page_va = page_va;
    g_pc.page_pa = page_pa;
    return page_pa + (addr & (PAGE_SIZE - 1));
}

static void init_Func(void) {
    kvar_lookup_name(memstart_addr);
    kvar_lookup_name(kimage_voffset);
    kfunc_lookup_name(__arch_copy_from_user);
    kfunc_lookup_name(__arch_copy_to_user);
    kfunc_lookup_name(copy_from_kernel_nofault);
    kfunc_lookup_name(copy_to_kernel_nofault);
    kfunc_lookup_name(find_task_by_vpid);
    kfunc_lookup_name(get_task_mm);
    kfunc_lookup_name(mmput);
    kfunc_lookup_name(pfn_valid);
}

static long init_pgtable(void) {
    uint64_t tcr_el1;
    asm volatile("mrs %0, tcr_el1" : "=r"(tcr_el1));
    uint64_t t1sz = (tcr_el1 >> 16) & 0x1F;
    uint64_t tg1 = tcr_el1 << 32 >> 62;
    my_va_bits = 64 - t1sz;
    my_page_shift = (tg1 == 1) ? 14 : (tg1 == 3) ? 16 : 12;

    uint64_t mmfr0;
    asm volatile("mrs %0, id_aa64mmfr0_el1" : "=r"(mmfr0));
    my_pa_bits = (mmfr0 & 0xF) == 6 ? 52 : 48;

    uint64_t ttbr1_el1;
    asm volatile("mrs %0, ttbr1_el1" : "=r"(ttbr1_el1));
    uint64_t pgd_pa = (ttbr1_el1 & 0xFFFFFFFFFFFE) & PAGE_MASK;

    struct mm_struct *init_mm = (struct mm_struct *)kallsyms_lookup_name("init_mm");
    if (init_mm) {
        for (uintptr_t i = (uintptr_t)init_mm; i < (uintptr_t)init_mm + 0xB0; i += 0x4) {
            if (*(uintptr_t *)i == __phys_to_kimg(pgd_pa)) {
                pgd_offset = i - (uintptr_t)init_mm;
                break;
            }
        }
    }

    for (int pid = 1; pid < 800; pid++) {
        struct task_struct *test_task = find_task_by_vpid(pid);
        if (!test_task) continue;

        struct mm_struct *real_mm = get_task_mm(test_task);
        if (!real_mm) continue;

        for (uint32_t offset = 0x100; offset < 0x5000; offset += 8) {
            uintptr_t val = 0;
            if (copy_from_kernel_nofault(&val, (void *)((uintptr_t)test_task + offset), 8) == 0) {
                if (val == (uintptr_t)real_mm) {
                    g_mm_offset = offset;
                    break;
                }
            }
        }
        mmput(real_mm);
        if (g_mm_offset != 0) break;
    }

    return 0;
}

static uint64_t *walk_pte_entry(uint64_t pgd, uint64_t va) {
    uint64_t pxd_bits = PAGE_SHIFT - 3, pxd_ptrs = 1u << pxd_bits;
    uint64_t pxd_va = pgd, pxd_pa = __virt_to_phys(pxd_va), pxd_entry_va = 0;

    for (int64_t lv = 4 - PAGE_LEVEL; lv < 4; lv++) {
        uint64_t pxd_shift = (PAGE_SHIFT - 3) * (4 - lv) + 3;
        uint64_t pxd_index = (va >> pxd_shift) & (pxd_ptrs - 1);
        pxd_entry_va = pxd_va + pxd_index * 8;

        uint64_t pxd_desc = 0;
        if (copy_from_kernel_nofault(&pxd_desc, (void *)pxd_entry_va, 8) != 0)
            return 0;

        if ((pxd_desc & 0b11) == 0b11) {
            pxd_pa = pxd_desc & (((1ul << (48 - PAGE_SHIFT)) - 1) << PAGE_SHIFT);
        } else if ((pxd_desc & 0b11) == 0b01) {
            uint64_t block_bits = (3 - lv) * pxd_bits + PAGE_SHIFT;
            pxd_pa = pxd_desc & (((1ul << (48 - block_bits)) - 1) << block_bits);
            pxd_va = __phys_to_virt(pxd_pa);
            break;
        } else {
            return 0;
        }
        pxd_va = __phys_to_virt(pxd_pa);
    }
    return (uint64_t *)pxd_entry_va;
}

static bool read_mem(pid_t pid, uintptr_t addr, void __user *buf, size_t size) {
    if (!buf || size == 0) return false;
    addr &= 0x00FFFFFFFFFFFFFFULL;

    size_t remain = size;
    uintptr_t cur = addr;
    void __user *cur_buf = buf;
    char temp_buf[4096];

    while (remain > 0) {
        size_t chunk = min((size_t)(PAGE_SIZE - (cur & (PAGE_SIZE - 1))), remain);
        if (chunk > 4096) chunk = 4096;

        phys_addr_t pa = virt_to_phys_cached(pid, cur);

        if (!pa || !pfn_valid(pa >> PAGE_SHIFT)) {
            memset(temp_buf, 0, chunk);
            __arch_copy_to_user(cur_buf, temp_buf, chunk);
        } else {
            void *kv = (void *)__phys_to_virt(pa & PAGE_MASK);
            if (copy_from_kernel_nofault(temp_buf, kv + (cur & ~PAGE_MASK), chunk) == 0) {
                __arch_copy_to_user(cur_buf, temp_buf, chunk);
            } else {
                memset(temp_buf, 0, chunk);
                __arch_copy_to_user(cur_buf, temp_buf, chunk);
            }
        }
        cur += chunk;
        cur_buf = (void __user *)((uintptr_t)cur_buf + chunk);
        remain -= chunk;
    }
    return true;
}

static bool write_mem(pid_t pid, uintptr_t addr, const void __user *buf, size_t size) {
    if (!buf || size == 0) return false;
    addr &= 0x00FFFFFFFFFFFFFFULL;

    size_t remain = size;
    uintptr_t cur = addr;
    const void __user *cur_buf = buf;
    char temp_buf[4096];

    while (remain > 0) {
        size_t chunk = min((size_t)(PAGE_SIZE - (cur & (PAGE_SIZE - 1))), remain);
        if (chunk > 4096) chunk = 4096;

        if (__arch_copy_from_user(temp_buf, cur_buf, chunk) != 0)
            return false;

        phys_addr_t pa = virt_to_phys_cached(pid, cur);
        if (!pa || !pfn_valid(pa >> PAGE_SHIFT))
            return false;

        void *kv = (void *)__phys_to_virt(pa & PAGE_MASK);
        if (copy_to_kernel_nofault(kv + (cur & ~PAGE_MASK), temp_buf, chunk) != 0)
            return false;

        cur += chunk;
        cur_buf = (const void __user *)((uintptr_t)cur_buf + chunk);
        remain -= chunk;
    }
    return true;
}

static void my_xcall(hook_fargs6_t *args, void *udata)
{
    uint64_t *raw = syscall_args(args);

    uint64_t magic = raw[0];
    if ((magic & 0xFFFFFFFFFFFFFF00ULL) != X_MAGIC)
        return;

    int cmd = magic & 0xFF;

    if (cmd == X_GET_VERSION) {
        args->ret = X_VERSION_RET;
        args->skip_origin = 1;
        return;
    }

    if (cmd == X_READ_MEMORY || cmd == X_WRITE_MEMORY) {
        uint64_t args_buf[4];
        args_buf[0] = raw[1];   /* pid */
        args_buf[1] = raw[2];   /* addr */
        args_buf[2] = raw[3];   /* buf */
        args_buf[3] = raw[4];   /* size */

        bool ok = (cmd == X_READ_MEMORY)
                    ? read_mem((pid_t)args_buf[0], args_buf[1], (void *)args_buf[2], args_buf[3])
                    : write_mem((pid_t)args_buf[0], args_buf[1], (void *)args_buf[2], args_buf[3]);

        args->ret = ok ? args_buf[3] : 0;
        args->skip_origin = 1;
    }
}

long dbc_rw_init(const char *args, const char *event, void *__user reserved)
{
    (void)reserved;
    kpm_demo_log_init("dbc-rw", event, args);

    g_pc.pid = 0;
    g_pc.task = NULL;
    g_pc.mm = NULL;
    g_pc.pgd = 0;
    g_pc.page_va = 0;
    g_pc.page_pa = 0;

    init_Func();
    init_pgtable();

    if (fp_hook_syscalln(41, 6, my_xcall, 0, 0) != 0) {
        pr_err("dbc-rw: hook syscall 41 failed\n");
        return -1;
    }
    pr_info("dbc-rw: ready syscall=41 magic=0x%llx ver=0x%x mm_off=%u pgd_off=%lx\n",
            (unsigned long long)X_MAGIC, (unsigned)X_VERSION_RET,
            g_mm_offset, (unsigned long)pgd_offset);
    return 0;
}

long dbc_rw_control0(const char *args, char *__user out_msg, int outlen)
{
    char buf[192];
    snprintf(buf, sizeof(buf),
             "dbc-rw ver=0x%x cache_pid=%d pgd=%lx page=%lx mm_off=%u",
             (unsigned)X_VERSION_RET, (int)g_pc.pid, (unsigned long)g_pc.pgd,
             (unsigned long)g_pc.page_va, g_mm_offset);
    (void)args;
    return kpm_demo_copy_message(buf, out_msg, outlen);
}

long dbc_rw_exit(void *__user reserved)
{
    (void)reserved;
    fp_unhook_syscalln(41, my_xcall, 0);
    proc_cache_clear();
    return kpm_demo_log_exit("dbc-rw");
}

#ifndef STEALTH_FUSED
KPM_INIT(dbc_rw_init);
KPM_CTL0(dbc_rw_control0);
KPM_EXIT(dbc_rw_exit);
#endif
