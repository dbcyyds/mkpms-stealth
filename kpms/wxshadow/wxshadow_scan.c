/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * W^X Shadow Memory KPM Module - Symbol Resolution and Offset Scanning
 *
 * Kernel symbol resolution, mm_struct/vma/task_struct offset detection.
 *
 * Copyright (C) 2024
 */

#include "wxshadow_internal.h"

/* Cached init_process pointer for use by other detection routines */
static void *wx_init_process = NULL;

/* ========== Safe symbol lookup (vmlinux only, no module traversal) ========== */

struct lookup_data {
    const char *name;
    unsigned long addr;
};

static int lookup_callback(void *data, const char *name, struct module *mod, unsigned long addr)
{
    struct lookup_data *ld = data;
    if (strcmp(name, ld->name) == 0) {
        ld->addr = addr;
        return 1; /* stop iteration */
    }
    return 0;
}

/*
 * Symbol lookup for KPM on modern GKI (6.6 + Clang CFI/LTO).
 *
 * Prefer kallsyms_lookup_name (KP always wires this). Try name.cfi_jt first
 * for CFI builds, then plain name.
 *
 * Avoid kallsyms_on_each_symbol by default: on some 6.6/GKI + LTO configs
 * the iterator is missing, weak, or ABI-mismatched and calling it panics
 * during module init (observed: stage=1 crash on resolve_symbols).
 */
static unsigned long lookup_name_safe(const char *name)
{
    unsigned long addr = 0;
    char cfi_name[128];
    int n;

    if (!name || !name[0] || !kallsyms_lookup_name)
        return 0;

    /* Clang CFI jump table (Android GKI / LTO) */
    n = 0;
    while (name[n] && n < (int)sizeof(cfi_name) - 16) {
        cfi_name[n] = name[n];
        n++;
    }
    /* append ".cfi_jt" */
    if (n < (int)sizeof(cfi_name) - 8) {
        cfi_name[n++] = '.';
        cfi_name[n++] = 'c';
        cfi_name[n++] = 'f';
        cfi_name[n++] = 'i';
        cfi_name[n++] = '_';
        cfi_name[n++] = 'j';
        cfi_name[n++] = 't';
        cfi_name[n] = '\0';
        addr = kallsyms_lookup_name(cfi_name);
        if (addr)
            return addr;
    }

    addr = kallsyms_lookup_name(name);
    if (addr)
        return addr;

    /*
     * Deliberately do NOT call kallsyms_on_each_symbol here.
     * On this device (6.6.89 GKI) stage=1 panic'd when iterating via
     * on_each_symbol during KPM init. kallsyms_lookup_name is sufficient.
     */
    (void)lookup_callback;
    return 0;
}

/* Try a list of symbol names; return first hit (0 if none). */
static unsigned long lookup_name_any(const char *const *names)
{
    unsigned long addr = 0;
    int i;

    if (!names)
        return 0;
    for (i = 0; names[i]; i++) {
        addr = lookup_name_safe(names[i]);
        if (addr) {
            pr_info("wxshadow: resolved '%s' -> %px\n", names[i], (void *)addr);
            return addr;
        }
    }
    return 0;
}

/* ========== Symbol resolution macros ========== */

/* Use lookup_name_safe for all symbol resolution to avoid module traversal hang */
#define RESOLVE_SYMBOL(name) \
    do { \
        kfunc_##name = (typeof(kfunc_##name))lookup_name_safe(#name); \
        if (!kfunc_##name) { \
            pr_err("wxshadow: failed to find symbol: %s\n", #name); \
            return -1; \
        } \
    } while (0)

#define RESOLVE_SYMBOL_OPTIONAL(name) \
    do { \
        kfunc_##name = (typeof(kfunc_##name))lookup_name_safe(#name); \
    } while (0)

/* ========== Symbol resolution ========== */

int resolve_symbols(void)
{
    pr_info("wxshadow: resolving symbols...\n");
    pr_info("wxshadow: kallsyms_lookup_name=%px on_each=%px\n",
            (void *)kallsyms_lookup_name, (void *)kallsyms_on_each_symbol);

    /* ===== Memory management (all exported) ===== */
    pr_info("wxshadow: [1/12] mm functions...\n");
    RESOLVE_SYMBOL(find_vma);
    pr_info("wxshadow: find_vma=%px\n", kfunc_find_vma);
    RESOLVE_SYMBOL(get_task_mm);
    pr_info("wxshadow: get_task_mm=%px\n", kfunc_get_task_mm);
    RESOLVE_SYMBOL(mmput);
    pr_info("wxshadow: mmput=%px\n", kfunc_mmput);
    /* find_task_by_vpid: use wxfunc(find_task_by_vpid) */

    /* exit_mmap - required, for proper cleanup on process exit */
    kfunc_exit_mmap = (void *)lookup_name_safe("exit_mmap");
    if (kfunc_exit_mmap) {
        pr_info("wxshadow: exit_mmap found at %px\n", kfunc_exit_mmap);
    } else {
        pr_err("wxshadow: exit_mmap not found, refusing to load without exit cleanup\n");
        return -ESRCH;
    }

    /* ===== Page allocation ===== */
    pr_info("wxshadow: [2/12] page alloc...\n");
    kfunc___get_free_pages = (typeof(kfunc___get_free_pages))
        lookup_name_safe("__get_free_pages");
    /* Do NOT fallback to get_zeroed_page — different ABI (1 arg vs 2). */
    if (!kfunc___get_free_pages) {
        pr_err("wxshadow: __get_free_pages not found\n");
        return -1;
    }
    pr_info("wxshadow: __get_free_pages=%px\n", kfunc___get_free_pages);

    pr_info("wxshadow: [3/12] page free...\n");
    kfunc_free_pages = (typeof(kfunc_free_pages))lookup_name_safe("free_pages");
    /* free_page(addr) is 1-arg; free_pages(addr, order) is 2-arg — keep exact. */
    if (!kfunc_free_pages) {
        pr_err("wxshadow: free_pages not found\n");
        return -1;
    }
    pr_info("wxshadow: free_pages=%px\n", kfunc_free_pages);

    /* ===== Address translation ===== */
    pr_info("wxshadow: [5/12] address translation...\n");
    kvar_memstart_addr = (s64 *)lookup_name_safe("memstart_addr");
    if (!kvar_memstart_addr) {
        pr_err("wxshadow: memstart_addr not found\n");
        return -1;
    }
    /* Do not blindly deref — use local copy after is_kva check */
    if (!is_kva((unsigned long)kvar_memstart_addr)) {
        pr_err("wxshadow: memstart_addr %px not a kva\n", kvar_memstart_addr);
        return -1;
    }
    pr_info("wxshadow: memstart_addr=%px, value=0x%llx\n",
            kvar_memstart_addr, (unsigned long long)(*kvar_memstart_addr));

    kvar_physvirt_offset = (s64 *)lookup_name_safe("physvirt_offset");
    if (kvar_physvirt_offset && is_kva((unsigned long)kvar_physvirt_offset)) {
        pr_info("wxshadow: physvirt_offset=%px, value=0x%llx (KASLR mode)\n",
                kvar_physvirt_offset,
                (unsigned long long)(*kvar_physvirt_offset));
    } else {
        kvar_physvirt_offset = NULL;
        pr_info("wxshadow: physvirt_offset not found, using traditional memstart_addr mode\n");
    }

    /* Determine PAGE_OFFSET based on VA bits from TCR_EL1 */
    {
        u64 tcr_el1_tmp;
        u64 t1sz_tmp, va_bits_tmp;
        unsigned long page_offset_mask;

        asm volatile("mrs %0, tcr_el1" : "=r"(tcr_el1_tmp));
        t1sz_tmp = (tcr_el1_tmp >> 16) & 0x3f;
        va_bits_tmp = 64 - t1sz_tmp;

        page_offset_base = ~0UL << (va_bits_tmp - 1);

        {
            unsigned long kaddr = (unsigned long)lookup_name_safe("_stext");
            if (kaddr) {
                page_offset_mask = ~0UL << (va_bits_tmp - 1);
                if ((kaddr & page_offset_mask) != page_offset_base) {
                    pr_warn("wxshadow: PAGE_OFFSET mismatch! calculated=0x%lx, from _stext=0x%lx\n",
                            page_offset_base, kaddr & page_offset_mask);
                    page_offset_base = kaddr & page_offset_mask;
                }
                pr_info("wxshadow: PAGE_OFFSET=0x%lx (va_bits=%lld, _stext=0x%lx)\n",
                        page_offset_base, va_bits_tmp, kaddr);
            } else {
                pr_info("wxshadow: PAGE_OFFSET=0x%lx (va_bits=%lld, calculated)\n",
                        page_offset_base, va_bits_tmp);
            }
        }
    }

    /*
     * physvirt detection order:
     * 1) physvirt_offset symbol (KASLR kernels)
     * 2) AT on a freshly allocated linear-map page (__get_free_pages)
     * 3) fall back to memstart_addr + PAGE_OFFSET formula
     *
     * Earlier we skipped AT during bring-up; symbol resolve is now stable
     * on 6.6.89 so re-enable the probe (required for correct PTE walks).
     */
    if (kvar_physvirt_offset) {
        detected_physvirt_offset = *kvar_physvirt_offset;
        physvirt_offset_valid = 1;
        pr_info("wxshadow: using physvirt_offset symbol = 0x%llx\n",
                (unsigned long long)detected_physvirt_offset);
    } else if (kfunc___get_free_pages && kfunc_free_pages) {
        unsigned long test_vaddr = kfunc___get_free_pages(0xcc0, 0);
        pr_info("wxshadow: AT probe page=%lx\n", test_vaddr);
        if (test_vaddr && is_kva(test_vaddr)) {
            unsigned long real_paddr = vaddr_to_paddr_at(test_vaddr);
            if (real_paddr) {
                detected_physvirt_offset = (s64)test_vaddr - (s64)real_paddr;
                physvirt_offset_valid = 1;
                pr_info("wxshadow: AT vaddr=%lx paddr=%lx physvirt=0x%llx\n",
                        test_vaddr, real_paddr,
                        (unsigned long long)detected_physvirt_offset);
            } else {
                pr_warn("wxshadow: AT failed for %lx\n", test_vaddr);
            }
            kfunc_free_pages(test_vaddr, 0);
        } else {
            pr_warn("wxshadow: AT probe alloc failed\n");
        }
    }
    if (!physvirt_offset_valid) {
        pr_warn("wxshadow: physvirt not detected; PTE walk may fail\n");
    }

    /* ===== Page table operations ===== */
    pr_info("wxshadow: [6/12] page table ops...\n");

    {
        u64 tcr_el1;
        u64 t1sz, tg1, va_bits;
        asm volatile("mrs %0, tcr_el1" : "=r"(tcr_el1));

        t1sz = (tcr_el1 >> 16) & 0x3f;
        va_bits = 64 - t1sz;

        tg1 = (tcr_el1 >> 30) & 0x3;
        wx_page_shift = 12;
        if (tg1 == 1) {
            wx_page_shift = 14;
        } else if (tg1 == 3) {
            wx_page_shift = 16;
        }

        wx_page_level = (va_bits - 4) / (wx_page_shift - 3);

        pr_info("wxshadow: TCR_EL1=0x%llx, va_bits=%lld, page_shift=%d, page_level=%d\n",
                tcr_el1, va_bits, wx_page_shift, wx_page_level);
    }

    /* Spinlock and task functions - using lookup_name_safe */
    wx__raw_spin_lock = (typeof(wx__raw_spin_lock))lookup_name_safe("_raw_spin_lock");
    wx__raw_spin_unlock = (typeof(wx__raw_spin_unlock))lookup_name_safe("_raw_spin_unlock");
    wx_find_task_by_vpid = (typeof(wx_find_task_by_vpid))lookup_name_safe("find_task_by_vpid");
    wx___task_pid_nr_ns = (typeof(wx___task_pid_nr_ns))lookup_name_safe("__task_pid_nr_ns");
    if (!wxfunc(_raw_spin_lock) || !wxfunc(_raw_spin_unlock) ||
        !wxfunc(find_task_by_vpid) || !wxfunc(__task_pid_nr_ns)) {
        pr_err("wxshadow: required kernel functions not found\n");
        return -1;
    }

    /* init_task - looked up via lookup_name_safe since framework doesn't export it */
    wx_init_task = (struct task_struct *)lookup_name_safe("init_task");
    if (!wx_init_task) {
        pr_err("wxshadow: init_task not found\n");
        return -1;
    }
    pr_info("wxshadow: wx_init_task at %px\n", wx_init_task);

    /* TLB flush - try flush_tlb_page first, fallback to __flush_tlb_range, then TLBI */
    kfunc_flush_tlb_page = (typeof(kfunc_flush_tlb_page))
        lookup_name_safe("flush_tlb_page");
    if (kfunc_flush_tlb_page) {
        pr_info("wxshadow: flush_tlb_page at %px\n", kfunc_flush_tlb_page);
    } else {
        /* flush_tlb_page is inline on some kernels, try __flush_tlb_range */
        kfunc___flush_tlb_range = (typeof(kfunc___flush_tlb_range))
            lookup_name_safe("__flush_tlb_range");
        if (kfunc___flush_tlb_range) {
            pr_info("wxshadow: using __flush_tlb_range at %px (fallback)\n", kfunc___flush_tlb_range);
        } else {
            /* Neither found - will use TLBI instruction fallback */
            pr_warn("wxshadow: neither flush_tlb_page nor __flush_tlb_range found\n");
            pr_info("wxshadow: will use TLBI instruction fallback (requires mm->context.id detection)\n");
        }
    }

    /* ===== THP split (optional) ===== */
    kfunc___split_huge_pmd = (typeof(kfunc___split_huge_pmd))
        lookup_name_safe("__split_huge_pmd");
    if (kfunc___split_huge_pmd) {
        pr_info("wxshadow: __split_huge_pmd at %px\n", kfunc___split_huge_pmd);
    } else {
        pr_info("wxshadow: __split_huge_pmd not found (THP disabled or inlined)\n");
    }

    /* ===== Cache operations ===== */
    pr_info("wxshadow: [7/12] cache ops...\n");
    /*
     * flush_dcache_page is unused at runtime (we clean via dc cvau on the
     * kernel VA of the shadow page). On 6.x/GKI it may be folio-only or
     * static — never hard-fail the module on it.
     */
    {
        static const char *const dcache_names[] = {
            "flush_dcache_page",
            "flush_dcache_folio",
            "__flush_dcache_page",
            NULL,
        };
        kfunc_flush_dcache_page =
            (typeof(kfunc_flush_dcache_page))lookup_name_any(dcache_names);
        if (!kfunc_flush_dcache_page)
            pr_info("wxshadow: flush_dcache_page not in kallsyms (ok, using dc cvau)\n");
    }

    {
        static const char *const icache_names[] = {
            "__flush_icache_range",
            "flush_icache_range",
            "__flush_cache_user_range",
            "invalidate_icache_range",
            "caches_clean_inval_pou",
            NULL,
        };
        kfunc___flush_icache_range =
            (typeof(kfunc___flush_icache_range))lookup_name_any(icache_names);
    }
    if (kfunc___flush_icache_range) {
        pr_info("wxshadow: using kernel icache flush at %px\n", kfunc___flush_icache_range);
    } else {
        pr_info("wxshadow: using built-in icache flush (dc cvau + ic ialluis)\n");
    }

    /* ===== Debug functions ===== */
    pr_info("wxshadow: [8/12] debug/single-step...\n");
    kfunc_user_enable_single_step = (typeof(kfunc_user_enable_single_step))
        lookup_name_safe("user_enable_single_step");
    kfunc_user_disable_single_step = (typeof(kfunc_user_disable_single_step))
        lookup_name_safe("user_disable_single_step");
    if (!kfunc_user_enable_single_step || !kfunc_user_disable_single_step) {
        pr_err("wxshadow: single step functions not found\n");
        return -1;
    }

    /* ===== BRK/Step hooks ===== */
    pr_info("wxshadow: [9/12] BRK/step hooks...\n");

    /*
     * On 6.6 arm64, brk_handler / single_step_handler are often static.
     * Prefer register_user_*_hook (exported). Direct hook is best-effort.
     */
    kfunc_brk_handler = (void *)lookup_name_safe("brk_handler");
    kfunc_single_step_handler = (void *)lookup_name_safe("single_step_handler");
    pr_info("wxshadow: brk_handler = %px\n", kfunc_brk_handler);
    pr_info("wxshadow: single_step_handler = %px\n", kfunc_single_step_handler);

    /* Register API — preferred on 6.6 when handlers are static */
    kfunc_register_user_break_hook =
        (typeof(kfunc_register_user_break_hook))lookup_name_safe("register_user_break_hook");
    kfunc_register_user_step_hook =
        (typeof(kfunc_register_user_step_hook))lookup_name_safe("register_user_step_hook");

    pr_info("wxshadow: register_user_break_hook = %px\n", kfunc_register_user_break_hook);
    pr_info("wxshadow: register_user_step_hook = %px\n", kfunc_register_user_step_hook);

    /* debug_hook_lock for safe manual unregister */
    kptr_debug_hook_lock = (spinlock_t *)lookup_name_safe("debug_hook_lock");
    pr_info("wxshadow: debug_hook_lock = %px\n", kptr_debug_hook_lock);

    /* Check if at least one method is available */
    if (!(kfunc_brk_handler && kfunc_single_step_handler) &&
        !(kfunc_register_user_break_hook && kfunc_register_user_step_hook)) {
        pr_err("wxshadow: neither direct hook nor register API available\n");
        return -1;
    }
    pr_info("wxshadow: [9/12] done\n");

    /* ===== Locking ===== */
    /* NOTE: mmap_lock and page_table_lock are NOT used - we operate locklessly */
    pr_info("wxshadow: [10/12] locking... (skipped - lockless operation)\n");

    /* ===== RCU ===== */
    pr_info("wxshadow: [11/12] RCU...\n");
    kfunc_rcu_read_lock = (typeof(kfunc_rcu_read_lock))
        lookup_name_safe("__rcu_read_lock");
    kfunc_rcu_read_unlock = (typeof(kfunc_rcu_read_unlock))
        lookup_name_safe("__rcu_read_unlock");
    kfunc_synchronize_rcu = (typeof(kfunc_synchronize_rcu))
        lookup_name_safe("synchronize_rcu");
    kfunc_kick_all_cpus_sync = (typeof(kfunc_kick_all_cpus_sync))
        lookup_name_safe("kick_all_cpus_sync");
    if (!kfunc_rcu_read_lock || !kfunc_rcu_read_unlock) {
        pr_err("wxshadow: RCU functions not found\n");
        return -1;
    }
    if (!kfunc_kick_all_cpus_sync) {
        /* Used only on unload drain; soft-fail so module can still load */
        pr_warn("wxshadow: kick_all_cpus_sync not found, unload drain may be weaker\n");
    }
    pr_info("wxshadow: synchronize_rcu = %px\n", kfunc_synchronize_rcu);
    pr_info("wxshadow: kick_all_cpus_sync = %px\n", kfunc_kick_all_cpus_sync);

    /* ===== Memory allocation ===== */
    pr_info("wxshadow: [12/12] memory alloc...\n");
    kfunc_kzalloc = (typeof(kfunc_kzalloc))lookup_name_safe("kzalloc");
    if (!kfunc_kzalloc)
        kfunc_kzalloc = (typeof(kfunc_kzalloc))lookup_name_safe("__kmalloc");
    if (!kfunc_kzalloc)
        kfunc_kzalloc = (typeof(kfunc_kzalloc))lookup_name_safe("__kmalloc_node");
    if (!kfunc_kzalloc)
        kfunc_kzalloc = (typeof(kfunc_kzalloc))lookup_name_safe("kmalloc_trace");
    if (!kfunc_kzalloc) {
        pr_err("wxshadow: kzalloc/__kmalloc not found\n");
        return -1;
    }
    pr_info("wxshadow: kzalloc resolved to %px\n", kfunc_kzalloc);

    /* Use lookup_name_safe to avoid module traversal hang */
    kfunc_kcalloc = (typeof(kfunc_kcalloc))lookup_name_safe("kcalloc");
    if (!kfunc_kcalloc)
        kfunc_kcalloc = (typeof(kfunc_kcalloc))lookup_name_safe("kmalloc_array");
    if (!kfunc_kcalloc) {
        pr_warn("wxshadow: kcalloc/kmalloc_array not found, will use kzalloc wrapper\n");
    } else {
        pr_info("wxshadow: kcalloc resolved to %px\n", kfunc_kcalloc);
    }

    kfunc_kfree = (typeof(kfunc_kfree))lookup_name_safe("kfree");
    if (!kfunc_kfree) {
        pr_err("wxshadow: kfree not found\n");
        return -1;
    }
    pr_info("wxshadow: kfree resolved to %px\n", kfunc_kfree);

    /* Safe memory access - try copy_from_kernel_nofault first, fallback to probe_kernel_read */
    kfunc_copy_from_kernel_nofault = (typeof(kfunc_copy_from_kernel_nofault))
        lookup_name_safe("copy_from_kernel_nofault");
    if (!kfunc_copy_from_kernel_nofault) {
        kfunc_copy_from_kernel_nofault = (typeof(kfunc_copy_from_kernel_nofault))
            lookup_name_safe("probe_kernel_read");
    }
    if (kfunc_copy_from_kernel_nofault) {
        pr_info("wxshadow: safe memory access available at %px\n", kfunc_copy_from_kernel_nofault);
    } else {
        pr_warn("wxshadow: copy_from_kernel_nofault not found, using direct access (less safe)\n");
    }

    /* copy_from_user removed: PATCH uses PTE walk instead (see copy_from_user_via_pte) */

    /* ===== Page fault handler (optional but critical for read-hide) ===== */
    /*
     * Linux 6.6 arm64: do_page_fault() is static and NOT in kallsyms.
     * Permission/access faults enter via do_mem_abort() -> fault_info[].fn.
     * Prefer do_mem_abort (global/exported-ish) over the static leaf.
     * Signature is still (far, esr, regs) for hook_wrap3.
     */
    pr_info("wxshadow: [13/14] page fault handler (6.x aware)...\n");
    {
        static const char *const fault_names[] = {
            "do_mem_abort",       /* best for 6.1+ (visible, all aborts) */
            "do_page_fault",      /* older kernels / non-static builds */
            "__do_page_fault",
            "do_translation_fault",
            NULL,
        };
        kfunc_do_page_fault = (void *)lookup_name_any(fault_names);
    }
    if (!kfunc_do_page_fault) {
        pr_warn("wxshadow: page fault handler not found, read hiding disabled\n");
    } else {
        pr_info("wxshadow: page fault handler found at %px\n", kfunc_do_page_fault);
    }

    /*
     * follow_page_pte is static in 6.x GUP and usually absent from kallsyms.
     * Only hook when the exact 5-arg symbol is present — do NOT hook
     * follow_page_mask / follow_page (different ABI).
     */
    pr_info("wxshadow: [14/14] follow_page_pte (GUP hiding)...\n");
    kfunc_follow_page_pte = (void *)lookup_name_safe("follow_page_pte");
    if (kfunc_follow_page_pte) {
        pr_info("wxshadow: follow_page_pte found at %px\n", kfunc_follow_page_pte);
    } else {
        pr_warn("wxshadow: follow_page_pte not in kallsyms (normal on 6.6), GUP hiding disabled\n");
    }

    /* dup_mmap for precise fork protection (real mm duplication only) */
    {
        static const char *const dup_names[] = {
            "dup_mmap",
            NULL,
        };
        kfunc_dup_mmap = (void *)lookup_name_any(dup_names);
    }
    if (kfunc_dup_mmap) {
        pr_info("wxshadow: dup_mmap found at %px\n", kfunc_dup_mmap);
    } else {
        pr_warn("wxshadow: dup_mmap not found, trying uprobe_dup_mmap\n");
    }

    kfunc_uprobe_dup_mmap = (void *)lookup_name_safe("uprobe_dup_mmap");
    if (kfunc_uprobe_dup_mmap) {
        pr_info("wxshadow: uprobe_dup_mmap found at %px\n", kfunc_uprobe_dup_mmap);
    } else {
        pr_warn("wxshadow: uprobe_dup_mmap not found\n");
    }

    /* init_task already resolved above via kallsyms */

    pr_info("wxshadow: all symbols resolved successfully\n");
    return 0;
}

/* ========== mm_struct offset scanning ========== */

/* Check if a kernel address is valid and readable */
static inline bool is_valid_kptr(unsigned long addr)
{
    u64 tmp;
    return safe_read_u64(addr, &tmp);
}

/* Safely read a string (up to maxlen bytes) */
static inline bool safe_read_str(unsigned long addr, char *buf, size_t maxlen)
{
    if (!is_kva(addr) || maxlen == 0)
        return false;

    if (kfunc_copy_from_kernel_nofault) {
        if (kfunc_copy_from_kernel_nofault(buf, (const void *)addr, maxlen) != 0)
            return false;
    } else {
        /* Fallback: byte-by-byte copy */
        size_t i;
        for (i = 0; i < maxlen; i++) {
            buf[i] = ((char *)addr)[i];
        }
    }
    buf[maxlen - 1] = '\0';
    return true;
}

int scan_mm_struct_offsets(void)
{
    /*
     * Use KP framework's mm_struct_offset.pgd_offset (linux/mm_types.h)
     * Framework detects this in resolve_mm_struct_offset() at boot time.
     */
    pr_info("wxshadow: using KP framework mm_struct_offset.pgd_offset = 0x%x\n",
            mm_struct_offset.pgd_offset);

    if (mm_struct_offset.pgd_offset < 0) {
        pr_err("wxshadow: KP framework did not detect pgd_offset!\n");
        return -1;
    }

    return 0;
}

/* ========== VMA offset scanning ========== */

int scan_vma_struct_offsets(void)
{
    void *mm;
    void *vma = NULL;
    int i;
    int found = 0;
    /*
     * Candidate user VAs to feed find_vma() — covers typical ELF load
     * bases, low mmap, and high canonical user space on 39/48-bit VA.
     */
    static const unsigned long probe_addrs[] = {
        0x0UL,
        0x10000UL,
        0x400000UL,
        0x8000UL,
        0x5500000000UL,
        0x7000000000UL,
        0x7f00000000UL,
        0x7ffffffff000UL,
    };
    unsigned int pi;

    pr_info("wxshadow: scanning vm_area_struct offsets (maple-tree safe)...\n");

    /*
     * Linux 6.1+ removed mm->mmap (linked list). VMAs live in mm->mm_mt
     * (maple tree). Never read *(mm) as the first VMA.
     * Always obtain a VMA via find_vma().
     */
    mm = kfunc_get_task_mm(current);
    if (!mm) {
        pr_warn("wxshadow: current task has no mm, using modern default vm_mm=0x10\n");
        goto use_default;
    }

    if (!kfunc_find_vma) {
        pr_warn("wxshadow: find_vma unavailable during VMA scan\n");
        kfunc_mmput(mm);
        goto use_default;
    }

    for (pi = 0; pi < sizeof(probe_addrs) / sizeof(probe_addrs[0]); pi++) {
        void *cand = kfunc_find_vma(mm, probe_addrs[pi]);
        if (!cand)
            continue;
        /* find_vma returns first VMA with vm_end > addr; require kva */
        if (!is_kva((unsigned long)cand))
            continue;
        vma = cand;
        pr_info("wxshadow: probe find_vma(%lx) -> vma=%px (vm_start=%lx vm_end=%lx)\n",
                probe_addrs[pi], vma,
                /* provisional offsets 0/8 are stable even with randomize_layout off */
                *(unsigned long *)cand,
                *((unsigned long *)cand + 1));
        break;
    }

    if (!vma) {
        pr_warn("wxshadow: find_vma returned no VMA, using default vm_mm offset\n");
        kfunc_mmput(mm);
        goto use_default;
    }

    pr_info("wxshadow: scanning VMA at %px for mm pointer %px\n", vma, mm);

    /*
     * vm_mm sits right after the vm_start/vm_end union on 6.1+ (offset 0x10)
     * when !RANDSTRUCT. Older linked-list layouts put it near 0x40.
     * Scan a wide range so both (and randomized builds that still match)
     * are covered.
     */
    for (i = 0x08; i < 0x100; i += 8) {
        u64 val;
        if (!safe_read_u64((unsigned long)vma + i, &val))
            continue;
        if (val == (u64)mm) {
            vma_vm_mm_offset = i;
            found = 1;
            pr_info("wxshadow: vm_area_struct.vm_mm offset: 0x%x\n",
                    vma_vm_mm_offset);
            break;
        }
    }

    kfunc_mmput(mm);

    if (!found) {
        pr_warn("wxshadow: vm_mm offset not found by search\n");
        goto use_default;
    }

    return 0;

use_default:
    /*
     * Linux 6.1+ / 6.6 default layout (no RANDSTRUCT):
     *   vm_start @ 0x00, vm_end @ 0x08, vm_mm @ 0x10
     * Pre-maple default was often 0x40 — wrong on 6.6 and causes silent
     * PTE/mm mismatches. Prefer the modern layout.
     */
    vma_vm_mm_offset = 0x10;
    pr_info("wxshadow: using default vm_mm offset: 0x%x (6.1+/maple layout)\n",
            vma_vm_mm_offset);
    return 0;
}

/* ========== task_struct offset detection ========== */

#define TASK_COMM_LEN 16
#define TASK_STRUCT_MAX_SIZE 0x1800

/* Find comm offset by searching for "swapper" or "swapper/0" in task_struct */
static int find_comm_offset(void *task)
{
    int i;
    char buf[16];

    for (i = 0x400; i < TASK_STRUCT_MAX_SIZE; i += 4) {
        /* Safely read potential comm string */
        if (!safe_read_str((unsigned long)task + i, buf, sizeof(buf)))
            continue;

        /* Check for "swapper" or "swapper/0" */
        if (buf[0] == 's' && buf[1] == 'w' && buf[2] == 'a' &&
            buf[3] == 'p' && buf[4] == 'p' && buf[5] == 'e' && buf[6] == 'r') {
            /* Verify it's null-terminated or followed by "/" */
            if (buf[7] == '\0' || (buf[7] == '/' && buf[8] == '0')) {
                pr_info("wxshadow: found comm at offset 0x%x: \"%.16s\"\n", i, buf);
                return i;
            }
        }
    }

    return -1;
}

int detect_task_struct_offsets(void)
{
    int search_start, search_end;
    int i;
    int16_t comm_offset;
    int16_t active_mm_off;

    pr_info("wxshadow: detecting task_struct offsets...\n");

    if (!wx_init_task) {
        pr_err("wxshadow: wx_init_task is NULL\n");
        return -1;
    }

    /* First, scan for comm_offset if not already set by framework */
    comm_offset = task_struct_offset.comm_offset;
    if (comm_offset <= 0) {
        comm_offset = find_comm_offset(wx_init_task);
        if (comm_offset > 0) {
            task_struct_offset.comm_offset = comm_offset;
            pr_info("wxshadow: comm_offset = 0x%x (scanned)\n", comm_offset);
        } else {
            pr_err("wxshadow: failed to find comm_offset\n");
            return -1;
        }
    } else {
        pr_info("wxshadow: comm_offset = 0x%x (from framework)\n", comm_offset);
    }

    /* Get active_mm_offset from framework */
    active_mm_off = task_struct_offset.active_mm_offset;

    /*
     * Detect tasks_offset based on active_mm_offset
     *
     * In Linux kernel task_struct layout, tasks (struct list_head) is
     * typically located before mm and active_mm fields:
     *   struct task_struct {
     *       ...
     *       struct list_head tasks;    <- tasks_offset
     *       ...
     *       struct mm_struct *mm;      <- mm_offset (active_mm - 8)
     *       struct mm_struct *active_mm; <- active_mm_offset
     *       ...
     *   }
     *
     * Search range: [active_mm_offset - 0x200, active_mm_offset)
     */
    if (active_mm_off > 0) {
        search_start = active_mm_off > 0x200 ? active_mm_off - 0x200 : 0x100;
        search_end = active_mm_off;
        pr_info("wxshadow: scanning tasks_offset based on active_mm_offset=0x%x, range=[0x%x, 0x%x)\n",
                active_mm_off, search_start, search_end);
    } else {
        /* Fallback: use comm_offset as upper bound */
        search_start = 0x100;
        search_end = comm_offset < 0x600 ? comm_offset : 0x600;
        pr_info("wxshadow: active_mm_offset not available, fallback range=[0x%x, 0x%x)\n",
                search_start, search_end);
    }

    /* Detect tasks_offset (not provided by framework) */
    for (i = search_start; i < search_end; i += sizeof(u64)) {
        unsigned long list_addr = (unsigned long)wx_init_task + i;
        u64 next_va, prev_va;

        /* Safely read list_head.next and list_head.prev */
        if (!safe_read_u64(list_addr, &next_va))
            continue;
        if (!safe_read_u64(list_addr + 8, &prev_va))
            continue;

        if (!is_kva(next_va) || !is_kva(prev_va))
            continue;

        if (next_va == prev_va)
            continue;

        /* Verify next->prev == self */
        {
            u64 next_prev;
            if (!safe_read_u64(next_va + 8, &next_prev))
                continue;
            if (next_prev != list_addr)
                continue;
        }

        /* Verify the candidate task has comm == "init" */
        {
            void *candidate = (void *)(next_va - i);
            char comm_buf[8];

            if (!safe_read_str((unsigned long)candidate + comm_offset, comm_buf, sizeof(comm_buf)))
                continue;

            if (comm_buf[0] == 'i' && comm_buf[1] == 'n' &&
                comm_buf[2] == 'i' && comm_buf[3] == 't') {
                task_struct_offset.tasks_offset = i;
                wx_init_process = candidate;
                pr_info("wxshadow: tasks_offset = 0x%x (based on active_mm_offset=0x%x)\n",
                        i, active_mm_off);
                break;
            }
        }
    }

    if (task_struct_offset.tasks_offset < 0) {
        pr_err("wxshadow: tasks_offset not found\n");
        return -1;
    }

    /*
     * Detect mm_offset using active_mm_offset from framework
     *
     * mm is always 8 bytes before active_mm in task_struct:
     *   struct mm_struct *mm;        <- mm_offset
     *   struct mm_struct *active_mm; <- active_mm_offset
     */
    if (task_struct_offset.active_mm_offset > 0) {
        task_struct_offset.mm_offset = task_struct_offset.active_mm_offset - 8;
        pr_info("wxshadow: mm_offset = 0x%x (active_mm_offset - 8)\n",
                task_struct_offset.mm_offset);
    } else {
        pr_err("wxshadow: active_mm_offset not available from framework\n");
        return -1;
    }

    /* pid/tgid: use wxfunc(__task_pid_nr_ns) */

    pr_info("wxshadow: task_struct offsets: tasks=0x%x, mm=0x%x, comm=0x%x\n",
            task_struct_offset.tasks_offset, task_struct_offset.mm_offset,
            task_struct_offset.comm_offset);
    pr_info("wxshadow: pid/tgid: using wxfunc(__task_pid_nr_ns)\n");

    return 0;
}

/* ========== mm->context.id offset scanning ========== */

/* ELF magic bytes */
#define ELFMAG0 0x7f
#define ELFMAG1 'E'
#define ELFMAG2 'L'
#define ELFMAG3 'F'

/*
 * Translate user VA to PA by walking mm's page table.
 * Uses TCR_EL1.T0SZ and TG0 for user space address translation.
 * Returns: PA on success, 0 on failure
 */
static unsigned long walk_pgtable_uaddr(void *mm, unsigned long uaddr)
{
    u64 *table;
    u64 desc;
    int level;
    u64 tcr;
    int t0sz, tg0;
    int granule_shift, stride;
    int va_bits, levels, start_level;

    /* Get PGD from mm - it's already a kernel virtual address */
    table = (u64 *)mm_pgd(mm);
    if (!table || !is_kva((unsigned long)table))
        return 0;

    /* Read TCR_EL1 to get T0SZ and TG0 */
    asm volatile("mrs %0, tcr_el1" : "=r"(tcr));

    t0sz = tcr & 0x3f;
    tg0 = (tcr >> 14) & 0x3;

    /* Decode TG0: 0=4KB, 1=64KB, 2=16KB */
    switch (tg0) {
    case 0:  /* 4KB */
        granule_shift = 12;
        stride = 9;
        break;
    case 1:  /* 64KB */
        granule_shift = 16;
        stride = 13;
        break;
    case 2:  /* 16KB */
        granule_shift = 14;
        stride = 11;
        break;
    default:
        granule_shift = 12;
        stride = 9;
    }

    va_bits = 64 - t0sz;
    levels = (va_bits - granule_shift + stride - 1) / stride;
    start_level = 4 - levels;

    for (level = start_level; level <= 3; level++) {
        int shift = granule_shift + stride * (3 - level);
        int idx = (uaddr >> shift) & ((1 << stride) - 1);

        /* Read descriptor directly (table is KVA) */
        if (!safe_read_u64((unsigned long)&table[idx], &desc))
            return 0;

        /* Check valid bit */
        if (!(desc & 1))
            return 0;

        unsigned long next_pa = desc & 0x0000FFFFFFFFF000UL;

        /* Check if table or block/page entry */
        if (level < 3 && (desc & 2)) {
            /* Table descriptor - convert PA to KVA for next level */
            table = (u64 *)phys_to_virt_safe(next_pa);
            if (!is_kva((unsigned long)table))
                return 0;
        } else {
            /* Block or page entry - translation complete */
            unsigned long offset_mask = (1UL << shift) - 1;
            return next_pa | (uaddr & offset_mask);
        }
    }

    return 0;
}

/*
 * Check if address contains ELF magic by walking mm's page table.
 * Returns: true if ELF magic found, false otherwise
 */
static bool check_elf_magic_at_uaddr(void *mm, unsigned long uaddr, int mm_offset)
{
    unsigned long pa, kva;
    unsigned char magic[4] = {0, 0, 0, 0};
    bool found;

    /* Must be a user address (not kernel) */
    if ((uaddr >> 48) != 0)
        return false;

    if (uaddr == 0)
        return false;

    /* Walk mm's page table to translate user VA to PA */
    pa = walk_pgtable_uaddr(mm, uaddr);
    if (pa == 0) {
        pr_info("wxshadow:   [0x%x] uaddr=0x%lx -> PA failed\n", mm_offset, uaddr);
        return false;
    }

    /* Convert PA to kernel VA */
    kva = phys_to_virt_safe(pa);
    if (!is_kva(kva)) {
        pr_info("wxshadow:   [0x%x] uaddr=0x%lx -> pa=0x%lx -> kva invalid\n",
                mm_offset, uaddr, pa);
        return false;
    }

    /* Read the first 4 bytes */
    if (kfunc_copy_from_kernel_nofault) {
        if (kfunc_copy_from_kernel_nofault(magic, (const void *)kva, 4) != 0) {
            pr_info("wxshadow:   [0x%x] uaddr=0x%lx -> kva=0x%lx read failed\n",
                    mm_offset, uaddr, kva);
            return false;
        }
    } else {
        magic[0] = ((unsigned char *)kva)[0];
        magic[1] = ((unsigned char *)kva)[1];
        magic[2] = ((unsigned char *)kva)[2];
        magic[3] = ((unsigned char *)kva)[3];
    }

    /* Check ELF magic */
    found = (magic[0] == ELFMAG0 && magic[1] == ELFMAG1 &&
             magic[2] == ELFMAG2 && magic[3] == ELFMAG3);

    pr_info("wxshadow:   [0x%x] uaddr=0x%lx -> magic=%02x %02x %02x %02x %s\n",
            mm_offset, uaddr, magic[0], magic[1], magic[2], magic[3],
            found ? "** ELF FOUND **" : "");

    return found;
}

/*
 * Scan mm->context.id by finding vdso (ELF magic pointer).
 * context.id is right before vdso in mm_context_t.
 *
 * mm_context_t layout:
 *   atomic64_t id;      <- context.id (what we want)
 *   void *vdso;         <- points to ELF magic
 *   ...
 *
 * Returns: context.id offset on success, -1 on failure
 */
static int scan_by_vdso_elf_magic(struct mm_struct *mm)
{
    int offset;
    int pgd_off = mm_struct_offset.pgd_offset;
    u64 val;
    int user_ptr_count = 0;

    if (pgd_off < 0) {
        pr_warn("wxshadow: pgd_offset not available\n");
        return -1;
    }

    pr_info("wxshadow: scanning for vdso (ELF magic) in mm=%px, pgd_offset=0x%x\n",
            mm, pgd_off);
    pr_info("wxshadow: search range: [0x%x, 0x%x)\n",
            pgd_off + 0x100, pgd_off + 0x400);

    /* Search for vdso pointer after pgd */
    for (offset = pgd_off + 0x100; offset < pgd_off + 0x400; offset += 8) {
        if (!safe_read_u64((unsigned long)mm + offset, &val))
            continue;

        /* Skip NULL and kernel addresses */
        if (val == 0 || (val >> 48) != 0)
            continue;

        /* Found a user-space pointer, check if it points to ELF magic */
        user_ptr_count++;
        if (check_elf_magic_at_uaddr(mm, val, offset)) {
            pr_info("wxshadow: === VDSO FOUND at mm+0x%x, vdso_addr=0x%llx ===\n",
                    offset, val);

            /* context.id is right before vdso (8 bytes) */
            return offset - 8;
        }
    }

    pr_warn("wxshadow: vdso not found (checked %d user pointers)\n", user_ptr_count);
    return -1;
}

/*
 * Scan mm->context.id offset using TTBR0_EL1 ASID (primary method).
 * Returns: offset on success, -1 on failure, -2 if ASID=0
 */
static int scan_by_ttbr0_asid(struct mm_struct *mm)
{
    u64 ttbr0_val, asid;
    int offset;

    /* Read TTBR0_EL1 to get ASID */
    asm volatile("mrs %0, ttbr0_el1" : "=r"(ttbr0_val));

    /* ASID is in bits [63:48] (16-bit ASID) or [55:48] (8-bit ASID) */
    asid = (ttbr0_val >> 48) & 0xFFFF;

    pr_info("wxshadow: TTBR0_EL1=0x%llx, ASID=%llu (0x%llx)\n", ttbr0_val, asid, asid);

    /* ASID=0 is problematic - too many zero fields would match */
    if (asid == 0) {
        pr_info("wxshadow: ASID=0, cannot use TTBR0 method\n");
        return -2;
    }

    /* Search for context.id in mm_struct */
    for (offset = 0x100; offset < 0x400; offset += 8) {
        u64 val;
        if (!safe_read_u64((unsigned long)mm + offset, &val))
            continue;

        /* Check if low 16 bits match ASID */
        if ((val & 0xFFFF) == asid) {
            pr_info("wxshadow: found mm->context.id at offset 0x%x, val=0x%llx (ASID match)\n",
                    offset, val);
            return offset;
        }
    }

    /* Try alternative: ASID might be in higher bits */
    for (offset = 0x100; offset < 0x400; offset += 8) {
        u64 val;
        if (!safe_read_u64((unsigned long)mm + offset, &val))
            continue;

        if (((val >> 48) & 0xFFFF) == asid ||
            ((val >> 32) & 0xFFFF) == asid) {
            pr_info("wxshadow: found mm->context.id at offset 0x%x (alt), val=0x%llx\n",
                    offset, val);
            return offset;
        }
    }

    pr_warn("wxshadow: TTBR0 method failed (ASID=%llu)\n", asid);
    return -1;
}

/*
 * Scan mm->context.id offset using TTBR0_EL1 ASID.
 * Must be called from user process context (not kernel thread).
 *
 * Returns: offset on success, -1 on failure
 */
static int scan_mm_context_id_offset_from_mm(struct mm_struct *mm)
{
    int offset;

    /* Validate mm pointer is readable */
    if (!is_valid_kptr((unsigned long)mm)) {
        pr_warn("wxshadow: invalid mm pointer: %px\n", mm);
        return -1;
    }

    /* Use TTBR0 ASID matching - only reliable method */
    pr_info("wxshadow: scanning mm->context.id using TTBR0 ASID method...\n");
    offset = scan_by_ttbr0_asid(mm);
    if (offset >= 0) {
        pr_info("wxshadow: mm_context_id_offset = 0x%x\n", offset);
        return offset;
    }

    pr_warn("wxshadow: TTBR0 ASID method failed (ASID may be 0 in kernel thread context)\n");
    return -1;
}

/*
 * Get init process (pid 1) mm_struct.
 * Uses wx_init_process cached from task_struct detection.
 */
static struct mm_struct *get_init_process_mm(void)
{
    struct task_struct *init_proc;
    struct mm_struct *mm = NULL;

    /* Use cached init_process from detect_task_struct_offsets */
    init_proc = wx_init_process;
    if (!init_proc) {
        pr_warn("wxshadow: init process not found\n");
        return NULL;
    }

    /* Get mm from init process */
    if (task_struct_offset.mm_offset >= 0) {
        safe_read_ptr((unsigned long)init_proc + task_struct_offset.mm_offset, (void **)&mm);
    }

    if (!mm) {
        pr_warn("wxshadow: init process has no mm\n");
        return NULL;
    }

    pr_info("wxshadow: init process mm=%px\n", mm);
    return mm;
}

/*
 * Try to scan mm->context.id offset.
 * Primary method: Find vdso (ELF magic) in init process mm, context.id is before it.
 * Fallback: TTBR0 ASID matching (requires user process context).
 *
 * Returns: 0 on success, -1 on failure (will retry later)
 */
int try_scan_mm_context_id_offset(void)
{
    struct mm_struct *mm;
    int offset;

    /* Already detected */
    if (mm_context_id_offset >= 0)
        return 0;

    pr_info("wxshadow: trying to scan mm->context.id offset...\n");

    /*
     * Method 1: Use init process (pid 1) mm and find vdso by ELF magic.
     * This works regardless of current context (kernel thread or user process).
     */
    mm = get_init_process_mm();
    if (mm) {
        offset = scan_by_vdso_elf_magic(mm);
        if (offset >= 0) {
            pr_info("wxshadow: mm_context_id_offset = 0x%x (vdso method)\n", offset);
            mm_context_id_offset = offset;
            return 0;
        }
    }

    /*
     * Method 2 (fallback): Use current process mm and TTBR0 ASID matching.
     * Only works in user process context.
     */
    if (task_struct_offset.mm_offset < 0) {
        pr_warn("wxshadow: mm_offset not detected\n");
        return -1;
    }

    if (!safe_read_ptr((unsigned long)current + task_struct_offset.mm_offset, (void **)&mm)) {
        pr_warn("wxshadow: failed to read mm from current task\n");
        return -1;
    }

    if (!mm) {
        pr_info("wxshadow: current is kernel thread, deferring to prctl\n");
        return -1;
    }

    offset = scan_mm_context_id_offset_from_mm(mm);
    if (offset >= 0) {
        mm_context_id_offset = offset;
        return 0;
    }

    /* Will retry at prctl time when in user process context */
    pr_info("wxshadow: context.id scan deferred to first prctl call\n");
    return -1;
}

/* ========== Debug: print tasks list ========== */

void debug_print_tasks_list(int max_count)
{
    struct task_struct *p;
    int count = 0;

    pr_info("wxshadow: === DEBUG: tasks list (first %d processes) ===\n", max_count);
    pr_info("wxshadow: task_struct_offset addr: %px\n", &task_struct_offset);
    pr_info("wxshadow: task_struct_offset: tasks=0x%x (%d), comm=0x%x (%d), mm=0x%x (%d)\n",
            (unsigned short)task_struct_offset.tasks_offset, task_struct_offset.tasks_offset,
            (unsigned short)task_struct_offset.comm_offset, task_struct_offset.comm_offset,
            (unsigned short)task_struct_offset.mm_offset, task_struct_offset.mm_offset);
    pr_info("wxshadow: pid/tgid: using wxfunc(__task_pid_nr_ns)\n");

    pr_info("wxshadow: wx_init_task = %px\n", wx_init_task);

    if (task_struct_offset.tasks_offset < 0 ||
        task_struct_offset.comm_offset < 0) {
        pr_err("wxshadow: tasks_offset (%d) or comm_offset (%d) not initialized!\n",
               task_struct_offset.tasks_offset, task_struct_offset.comm_offset);
        return;
    }

    if (!wx_init_task) {
        pr_err("wxshadow: wx_init_task is NULL!\n");
        return;
    }

    pr_info("wxshadow: wx_init_task (swapper) at %px\n", wx_init_task);

    /* Iterate using wx_next_task() - fixed implementation in wxshadow_internal.h */
    for (p = wx_init_task; (p = wx_next_task(p)) != wx_init_task && count < max_count; ) {
        pid_t pid = 0;
        pid_t tgid = 0;
        const char *comm;
        void *mm = NULL;

        /* Use wxfunc(__task_pid_nr_ns) */
        pid = wxfunc(__task_pid_nr_ns)(p, PIDTYPE_PID, NULL);
        tgid = wxfunc(__task_pid_nr_ns)(p, PIDTYPE_TGID, NULL);

        /* Use get_task_comm helper from linux/sched.h */
        comm = get_task_comm(p);

        if (task_struct_offset.mm_offset >= 0) {
            safe_read_ptr((unsigned long)p + task_struct_offset.mm_offset, &mm);
        }

        pr_info("wxshadow: [%d] task=%px pid=%d tgid=%d mm=%px comm=\"%.16s\"\n",
                count, p, pid, tgid, mm, comm ? comm : "(null)");

        count++;
    }

    pr_info("wxshadow: === END tasks list (%d processes printed) ===\n", count);
}
