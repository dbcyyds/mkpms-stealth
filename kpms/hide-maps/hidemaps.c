/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * hide-maps — maps 隐藏 + 用户态无痕注入接口
 *
 * prctl 用户态 API（不经 kpatch ctl）:
 *   PR_HIDEMAPS_PING      0x484D0001  → 返回 0x484D 表示模块在
 *   PR_HIDEMAPS_ADD_KW    0x484D0002  arg1=用户串，增加 maps 隐藏词
 *   PR_HIDEMAPS_ADD_RANGE 0x484D0003  arg1=start, arg2=end  按 VMA 地址藏（匿名段/stub）
 *   PR_HIDEMAPS_CLR_RANGE 0x484D0004  清空地址区间表
 *   PR_HIDEMAPS_REGISTER  0x484D0010  arg1=pkg, arg2=so
 *   PR_HIDEMAPS_QUERY     0x484D0011
 *   PR_HIDEMAPS_CLEAR     0x484D0012  清 job（不清 kw/range，用 CLR_RANGE）
 *
 * 用户态工具 stealth_inject：只输包名+so路径，进程一出现即注入并靠本模块藏 maps。
 */

#include <compiler.h>
#include <kpmodule.h>
#include <linux/printk.h>
#include <common.h>
#include <kputils.h>
#include <linux/string.h>
#include <hook.h>
#include <syscall.h>
#include <uapi/asm-generic/unistd.h>
#include "../common/kpm_demo_helpers.h"

#ifndef STEALTH_FUSED
KPM_MODULE_INFO("hide-maps",
                "1.5.0",
                "GPL v2",
                "dbc",
                "maps/smaps/map_files hide by keyword+addr range");
#endif

/* ---- prctl ABI（用户态工具共用） ---- */
#define PR_HIDEMAPS_PING       0x484D0001
#define PR_HIDEMAPS_ADD_KW     0x484D0002
#define PR_HIDEMAPS_ADD_RANGE  0x484D0003  /* start, end (end exclusive) */
#define PR_HIDEMAPS_CLR_RANGE  0x484D0004
#define PR_HIDEMAPS_REGISTER   0x484D0010
#define PR_HIDEMAPS_QUERY      0x484D0011
#define PR_HIDEMAPS_CLEAR      0x484D0012
#define HIDEMAPS_MAGIC         0x484D

#define MAX_KEYWORDS   48
#define MAX_KW_LEN     128
#define MAX_RANGES     96
#define MAX_LINE       512
#define GFP_KERNEL_VAL 0xcc0
/* PTR_ERR 范围：返回值 >= -4095 的指针视为错误码 */
#define HM_IS_ERR_PTR(x) ((unsigned long)(x) >= (unsigned long)-4095UL)
#define HM_ERR_PTR(e)    ((unsigned long)(long)(e))

#ifndef __NR_prctl
#define __NR_prctl 167
#endif

struct seq_file_min {
    char *buf;
    size_t size;
    size_t from;
    size_t count;
};

/* 与用户态 stealth_inject.h 保持一致 */
struct hidemaps_job {
    char package[128];
    char so_path[256];
    int  flags;      /* bit0: auto-start 仅用户态用 */
    int  last_pid;   /* 内核可写：最近一次 QUERY 时用户态回填用 */
    int  status;     /* 0 idle 1 armed 2 injected */
};

static void *(*k_kzalloc)(size_t size, unsigned int flags);
static void (*k_kfree)(const void *addr);
static unsigned long (*k_copy_from_user)(void *to, const void __user *from, unsigned long n);
static unsigned long (*k_copy_to_user)(void __user *to, const void *from, unsigned long n);

static void *sym_show_map_vma;
static void *sym_show_smap;
static void *sym_proc_map_files_readdir;
static void *sym_filldir64;
static void *sym_map_files_get_link;
static void *sym_dentry_path_raw;
static int g_hooked_vma;
static int g_hooked_smap;
static int g_hooked_prctl;
static int g_hooked_mf_readdir;
static int g_hooked_filldir64;
static int g_hooked_mf_get_link;

/* map_files readdir 嵌套深度：仅在此期间过滤 filldir64 */
static volatile int g_mf_readdir_nest;

static char g_keywords[MAX_KEYWORDS][MAX_KW_LEN];
static int g_nr_kw;

/* 按地址隐藏：用于匿名 r-xp stub、注入 so 段（路径可已 unlink） */
struct hide_range {
    unsigned long start;
    unsigned long end; /* exclusive */
};
static struct hide_range g_ranges[MAX_RANGES];
static int g_nr_range;

static struct hidemaps_job g_job;
static int g_job_valid;

/* 注意：关键词用 strstr 匹配整行，切勿加包名/过短串（会误伤 /data/app/<pkg>/libUE4 等） */
static const char *const k_defaults[] = {
    "libdbc.so", "libdbc", "libdbc_fl", "framelog",
    ".kload", "tinject", "libtcore.so", "libtcore",
    "memfd:zygisk", "zygisk-module", "DbcHK",
    NULL,
};

static void reset_defaults(void)
{
    int i;
    g_nr_kw = 0;
    for (i = 0; k_defaults[i] && g_nr_kw < MAX_KEYWORDS; i++) {
        strncpy(g_keywords[g_nr_kw], k_defaults[i], MAX_KW_LEN - 1);
        g_keywords[g_nr_kw][MAX_KW_LEN - 1] = '\0';
        g_nr_kw++;
    }
}

static int keyword_exists(const char *s)
{
    int i;
    for (i = 0; i < g_nr_kw; i++)
        if (!strcmp(g_keywords[i], s))
            return 1;
    return 0;
}

static int add_keyword(const char *s)
{
    if (!s || !s[0] || strlen(s) >= MAX_KW_LEN)
        return -1;
    if (keyword_exists(s))
        return 0;
    if (g_nr_kw >= MAX_KEYWORDS)
        return -1;
    strncpy(g_keywords[g_nr_kw], s, MAX_KW_LEN - 1);
    g_keywords[g_nr_kw][MAX_KW_LEN - 1] = '\0';
    g_nr_kw++;
    pr_info("hide-maps: +kw '%s' nr=%d\n", s, g_nr_kw);
    return 0;
}

/* 从 so 路径提取文件名加入关键词 */
static void add_kw_from_path(const char *path)
{
    const char *base;
    char name[MAX_KW_LEN];
    int n;

    if (!path || !path[0])
        return;
    base = strrchr(path, '/');
    base = base ? base + 1 : path;
    n = 0;
    while (base[n] && base[n] != ' ' && n < MAX_KW_LEN - 1) {
        name[n] = base[n];
        n++;
    }
    name[n] = '\0';
    if (n > 0)
        add_keyword(name);
    /* 无后缀名也藏 */
    if (n > 3 && name[n - 3] == '.' && name[n - 2] == 's' && name[n - 1] == 'o') {
        name[n - 3] = '\0';
        if (name[0])
            add_keyword(name);
    }
}

static void parse_csv_keywords(const char *args)
{
    char tmp[256];
    char *p, *tok;

    if (!args || !args[0])
        return;
    strncpy(tmp, args, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';
    p = tmp;
    while ((tok = strsep(&p, ",; \t")) != NULL) {
        if (tok[0])
            add_keyword(tok);
    }
}

static int range_exists(unsigned long start, unsigned long end)
{
    int i;
    for (i = 0; i < g_nr_range; i++)
        if (g_ranges[i].start == start && g_ranges[i].end == end)
            return 1;
    return 0;
}

/* 合并重叠/相邻区间，减少条目 */
static void ranges_coalesce(void)
{
    int i, j;
    struct hide_range t;

    if (g_nr_range < 2)
        return;
    /* 简单冒泡按 start 排序 */
    for (i = 0; i < g_nr_range; i++) {
        for (j = i + 1; j < g_nr_range; j++) {
            if (g_ranges[j].start < g_ranges[i].start) {
                t = g_ranges[i];
                g_ranges[i] = g_ranges[j];
                g_ranges[j] = t;
            }
        }
    }
    j = 0;
    for (i = 1; i < g_nr_range; i++) {
        if (g_ranges[i].start <= g_ranges[j].end) {
            if (g_ranges[i].end > g_ranges[j].end)
                g_ranges[j].end = g_ranges[i].end;
        } else {
            j++;
            g_ranges[j] = g_ranges[i];
        }
    }
    g_nr_range = j + 1;
}

static int add_range(unsigned long start, unsigned long end)
{
    if (!start || end <= start)
        return -22; /* EINVAL */
    /* 页对齐放宽：允许任意，调用方负责 */
    if (range_exists(start, end))
        return 0;
    if (g_nr_range >= MAX_RANGES)
        return -28; /* ENOSPC */
    g_ranges[g_nr_range].start = start;
    g_ranges[g_nr_range].end = end;
    g_nr_range++;
    ranges_coalesce();
    pr_info("hide-maps: +range [%lx,%lx) nr=%d\n", start, end, g_nr_range);
    return 0;
}

static void clear_ranges(void)
{
    g_nr_range = 0;
    pr_info("hide-maps: ranges cleared\n");
}

static int vma_in_hide_range(unsigned long s, unsigned long e)
{
    int i;
    for (i = 0; i < g_nr_range; i++) {
        /* 区间相交则藏 */
        if (s < g_ranges[i].end && e > g_ranges[i].start)
            return 1;
    }
    return 0;
}

static int line_should_hide(const char *line)
{
    int i;
    unsigned long s = 0, e = 0;

    /* 1) 地址区间（匿名 stub / 注入 so 段） */
    if (line[0] && ((line[0] >= '0' && line[0] <= '9') ||
                    (line[0] >= 'a' && line[0] <= 'f') ||
                    (line[0] >= 'A' && line[0] <= 'F'))) {
        /* maps 行: start-end perms ... */
        if (sscanf(line, "%lx-%lx", &s, &e) == 2 && e > s &&
            vma_in_hide_range(s, e))
            return 1;
    }

    /* 2) 关键词 */
    for (i = 0; i < g_nr_kw; i++) {
        if (g_keywords[i][0] && strstr(line, g_keywords[i]))
            return 1;
    }
    /* 已登记任务的 so 路径片段 */
    if (g_job_valid && g_job.so_path[0] && strstr(line, g_job.so_path))
        return 1;
    return 0;
}

static void *alloc_line(size_t n)
{
    if (k_kzalloc)
        return k_kzalloc(n, GFP_KERNEL_VAL);
    return NULL;
}

static void free_line(void *p)
{
    if (p && k_kfree)
        k_kfree(p);
}

static void filter_seq_line(hook_fargs2_t *args)
{
    struct seq_file_min *m;
    size_t start, end, len;
    char *line;

    m = (struct seq_file_min *)args->arg0;
    if (!m || !m->buf)
        return;
    start = (size_t)args->local.data0;
    end = m->count;
    if (end <= start || end - start > MAX_LINE)
        return;
    len = end - start;
    line = alloc_line(len + 1);
    if (!line)
        return;
    memcpy(line, m->buf + start, len);
    line[len] = '\0';
    if (line_should_hide(line))
        m->count = start;
    free_line(line);
}

static void show_map_before(hook_fargs2_t *args, void *udata)
{
    struct seq_file_min *m = (struct seq_file_min *)args->arg0;
    (void)udata;
    args->local.data0 = m ? m->count : 0;
}

static void show_map_after(hook_fargs2_t *args, void *udata)
{
    (void)udata;
    filter_seq_line(args);
}

/*
 * map_files 隐藏策略（稳妥、只用 before / skip_origin）：
 *  1) proc_map_files_readdir before/after 维护 nest 计数
 *  2) filldir64 before：nest>0 且 name 为 start-end 且落在 hide range → 不 emit
 *  3) map_files_get_link / proc_map_files_get_link before：区间命中 → -ENOENT
 * 不做 get_link 的 after 回调（历史上 after 不稳定）。
 * 关键词路径：依赖用户态 ADD_RANGE 覆盖 so 段；map_files 以地址过滤为主。
 */

static int parse_hex_range(const char *name, int namelen, unsigned long *s,
                           unsigned long *e)
{
    char tmp[48];
    int i, n;

    if (!name || namelen <= 2 || namelen >= (int)sizeof(tmp))
        return 0;
    for (i = 0; i < namelen; i++) {
        char c = name[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
              (c >= 'A' && c <= 'F') || c == '-'))
            return 0;
    }
    memcpy(tmp, name, (size_t)namelen);
    tmp[namelen] = '\0';
    n = 0;
    *s = 0;
    *e = 0;
    /* 简易 %lx-%lx，避免 sscanf 依赖 */
    {
        const char *p = tmp;
        unsigned long v = 0;
        int part = 0;
        for (; *p; p++) {
            char c = *p;
            if (c == '-') {
                if (part != 0)
                    return 0;
                *s = v;
                v = 0;
                part = 1;
                continue;
            }
            v <<= 4;
            if (c >= '0' && c <= '9')
                v |= (unsigned long)(c - '0');
            else if (c >= 'a' && c <= 'f')
                v |= (unsigned long)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F')
                v |= (unsigned long)(c - 'A' + 10);
            n++;
        }
        if (part != 1 || n < 3)
            return 0;
        *e = v;
    }
    return *e > *s;
}

static void mf_readdir_before(hook_fargs2_t *args, void *udata)
{
    (void)args;
    (void)udata;
    g_mf_readdir_nest++;
}

static void mf_readdir_after(hook_fargs2_t *args, void *udata)
{
    (void)args;
    (void)udata;
    if (g_mf_readdir_nest > 0)
        g_mf_readdir_nest--;
}

/*
 * filldir64(ctx, name, namelen, offset, ino, d_type)
 * 返回 0 表示该条目处理成功；skip 时返回 0 且不调用 origin = 不写入 dent。
 */
static void filldir64_before(hook_fargs6_t *args, void *udata)
{
    const char *name;
    int namelen;
    unsigned long s = 0, e = 0;

    (void)udata;
    if (g_mf_readdir_nest <= 0)
        return;
    name = (const char *)args->arg1;
    namelen = (int)args->arg2;
    if (!parse_hex_range(name, namelen, &s, &e))
        return;
    if (!vma_in_hide_range(s, e))
        return;
    args->skip_origin = 1;
    args->ret = 0;
}

/*
 * map_files_get_link(dentry, inode, delayed_call) → const char *
 * 失败返回 ERR_PTR(-ENOENT) ≈ (void *)-2
 */
static void mf_get_link_before(hook_fargs3_t *args, void *udata)
{
    void *dentry = (void *)args->arg0;
    char buf[96];
    char *p;
    const char *base;
    unsigned long s = 0, e = 0;
    int len;

    (void)udata;
    if (!dentry)
        return;

    /* 优先 dentry_path_raw 取末段名；失败则放弃（不瞎猜 dentry 布局） */
    if (sym_dentry_path_raw) {
        typedef char *(*dpr_t)(const void *, char *, int);
        p = ((dpr_t)sym_dentry_path_raw)(dentry, buf, (int)sizeof(buf));
        if (!p || HM_IS_ERR_PTR(p))
            return;
        base = strrchr(p, '/');
        base = base ? base + 1 : p;
        len = (int)strlen(base);
    } else {
        return;
    }

    if (!parse_hex_range(base, len, &s, &e))
        return;
    if (!vma_in_hide_range(s, e))
        return;
    args->skip_origin = 1;
    /* ERR_PTR(-ENOENT), ENOENT=2 */
    args->ret = (unsigned long)(long)-2;
}

/* ---------- prctl 接口 ---------- */

static int copy_cstr_from_user(char *dst, size_t dstlen, unsigned long uptr)
{
    long n;
    if (!uptr || dstlen == 0)
        return -1;
    /* compat_strncpy_from_user 若可用 */
    n = compat_strncpy_from_user(dst, (const char __user *)uptr, dstlen);
    if (n <= 0)
        return -1;
    dst[dstlen - 1] = '\0';
    return 0;
}

static void prctl_before(hook_fargs4_t *args, void *udata)
{
    int option = (int)syscall_argn(args, 0);
    unsigned long a1 = syscall_argn(args, 1);
    unsigned long a2 = syscall_argn(args, 2);
    struct hidemaps_job tmp;
    char kw[MAX_KW_LEN];

    (void)udata;

    if (option < PR_HIDEMAPS_PING || option > PR_HIDEMAPS_CLEAR)
        return;

    switch (option) {
    case PR_HIDEMAPS_PING:
        args->ret = HIDEMAPS_MAGIC;
        args->skip_origin = 1;
        break;

    case PR_HIDEMAPS_ADD_KW:
        if (copy_cstr_from_user(kw, sizeof(kw), a1) < 0) {
            args->ret = -14;
        } else {
            args->ret = add_keyword(kw);
        }
        args->skip_origin = 1;
        break;

    case PR_HIDEMAPS_ADD_RANGE:
        /* prctl(ADD_RANGE, start, end, 0, 0) — end exclusive */
        args->ret = add_range(a1, a2);
        args->skip_origin = 1;
        break;

    case PR_HIDEMAPS_CLR_RANGE:
        clear_ranges();
        args->ret = 0;
        args->skip_origin = 1;
        break;

    case PR_HIDEMAPS_REGISTER:
        /*
         * 用户态: prctl(REGISTER, pkg_str, so_str, flags, 0)
         * 用 strncpy_from_user，避免 struct copy_from_user 符号差异
         */
        memset(&tmp, 0, sizeof(tmp));
        if (copy_cstr_from_user(tmp.package, sizeof(tmp.package), a1) < 0 ||
            copy_cstr_from_user(tmp.so_path, sizeof(tmp.so_path), a2) < 0) {
            args->ret = -14;
            args->skip_origin = 1;
            break;
        }
        tmp.flags = (int)syscall_argn(args, 3);
        if (!tmp.package[0] || !tmp.so_path[0]) {
            args->ret = -22;
            args->skip_origin = 1;
            break;
        }
        memcpy(&g_job, &tmp, sizeof(g_job));
        g_job.status = 1; /* armed */
        g_job.last_pid = 0;
        g_job_valid = 1;
        /* 只藏 so 路径/文件名，不要把包名当关键词（会误伤 /data/app/包名/ 等行） */
        add_kw_from_path(g_job.so_path);
        if (strrchr(g_job.so_path, '/'))
            add_keyword(g_job.so_path); /* 完整路径也藏 */
        pr_info("hide-maps: REGISTER pkg=%s so=%s flags=%d\n",
                g_job.package, g_job.so_path, g_job.flags);
        args->ret = 0;
        args->skip_origin = 1;
        break;

    case PR_HIDEMAPS_QUERY:
        /* 返回 status；可选 a1 非 0 时仅表示查询 */
        args->ret = g_job_valid ? g_job.status : 0;
        args->skip_origin = 1;
        break;

    case PR_HIDEMAPS_CLEAR:
        memset(&g_job, 0, sizeof(g_job));
        g_job_valid = 0;
        pr_info("hide-maps: job cleared\n");
        args->ret = 0;
        args->skip_origin = 1;
        break;

    default:
        break;
    }
}

static int resolve_syms(void)
{
    k_kzalloc = (void *)kallsyms_lookup_name("kzalloc");
    if (!k_kzalloc)
        k_kzalloc = (void *)kallsyms_lookup_name("__kmalloc");
    k_kfree = (void *)kallsyms_lookup_name("kfree");
    k_copy_from_user = (void *)kallsyms_lookup_name("copy_from_user");
    k_copy_to_user = (void *)kallsyms_lookup_name("copy_to_user");

    if (!k_kzalloc || !k_kfree) {
        pr_err("hide-maps: alloc symbols missing\n");
        return -1;
    }
    return 0;
}

static int install_hooks(void)
{
    int n = 0;

    sym_show_map_vma = (void *)kallsyms_lookup_name("show_map_vma");
    if (sym_show_map_vma &&
        hook_wrap2(sym_show_map_vma, show_map_before, show_map_after, NULL) == 0) {
        g_hooked_vma = 1;
        n++;
        pr_info("hide-maps: hooked show_map_vma %px\n", sym_show_map_vma);
    } else {
        pr_err("hide-maps: show_map_vma hook failed\n");
    }

    sym_show_smap = (void *)kallsyms_lookup_name("show_smap");
    if (sym_show_smap &&
        hook_wrap2(sym_show_smap, show_map_before, show_map_after, NULL) == 0) {
        g_hooked_smap = 1;
        n++;
        pr_info("hide-maps: hooked show_smap %px\n", sym_show_smap);
    }

    if (hook_syscalln(__NR_prctl, 5, prctl_before, NULL, NULL) == 0) {
        g_hooked_prctl = 1;
        n++;
        pr_info("hide-maps: hooked prctl (userspace API)\n");
    } else {
        pr_err("hide-maps: prctl hook failed\n");
    }

    /* map_files：readdir 过滤 + get_link 拒绝（可选，失败不致命） */
    sym_proc_map_files_readdir =
        (void *)kallsyms_lookup_name("proc_map_files_readdir");
    sym_filldir64 = (void *)kallsyms_lookup_name("filldir64");
    sym_map_files_get_link = (void *)kallsyms_lookup_name("map_files_get_link");
    if (!sym_map_files_get_link)
        sym_map_files_get_link =
            (void *)kallsyms_lookup_name("proc_map_files_get_link");
    sym_dentry_path_raw = (void *)kallsyms_lookup_name("dentry_path_raw");

    g_mf_readdir_nest = 0;
    if (sym_proc_map_files_readdir &&
        hook_wrap2(sym_proc_map_files_readdir, mf_readdir_before,
                   mf_readdir_after, NULL) == 0) {
        g_hooked_mf_readdir = 1;
        pr_info("hide-maps: hooked proc_map_files_readdir %px\n",
                sym_proc_map_files_readdir);
    } else {
        pr_warn("hide-maps: proc_map_files_readdir hook skip\n");
    }

    if (g_hooked_mf_readdir && sym_filldir64 &&
        hook_wrap6(sym_filldir64, filldir64_before, NULL, NULL) == 0) {
        g_hooked_filldir64 = 1;
        pr_info("hide-maps: hooked filldir64 (map_files filter) %px\n",
                sym_filldir64);
    } else if (g_hooked_mf_readdir) {
        pr_warn("hide-maps: filldir64 hook failed — map_files readdir 可能仍露名\n");
    }

    if (sym_map_files_get_link && sym_dentry_path_raw &&
        hook_wrap3(sym_map_files_get_link, mf_get_link_before, NULL, NULL) ==
            0) {
        g_hooked_mf_get_link = 1;
        pr_info("hide-maps: hooked map_files get_link %px\n",
                sym_map_files_get_link);
    } else {
        pr_warn("hide-maps: map_files get_link hook skip (dpr=%px gl=%px)\n",
                sym_dentry_path_raw, sym_map_files_get_link);
    }

    return n >= 2 ? 0 : -1; /* 至少 maps + prctl；map_files 失败不拦加载 */
}

static void remove_hooks(void)
{
    if (g_hooked_filldir64 && sym_filldir64) {
        hook_unwrap(sym_filldir64, filldir64_before, NULL);
        g_hooked_filldir64 = 0;
    }
    if (g_hooked_mf_readdir && sym_proc_map_files_readdir) {
        hook_unwrap(sym_proc_map_files_readdir, mf_readdir_before,
                    mf_readdir_after);
        g_hooked_mf_readdir = 0;
    }
    if (g_hooked_mf_get_link && sym_map_files_get_link) {
        hook_unwrap(sym_map_files_get_link, mf_get_link_before, NULL);
        g_hooked_mf_get_link = 0;
    }
    if (g_hooked_vma && sym_show_map_vma)
        unhook(sym_show_map_vma);
    if (g_hooked_smap && sym_show_smap)
        unhook(sym_show_smap);
    if (g_hooked_prctl)
        unhook_syscalln(__NR_prctl, prctl_before, NULL);
    g_hooked_vma = g_hooked_smap = g_hooked_prctl = 0;
    g_mf_readdir_nest = 0;
}

long hide_maps_init(const char *args, const char *event, void *__user reserved)
{
    (void)reserved;
    kpm_demo_log_init("hide-maps", event, args);
    memset(&g_job, 0, sizeof(g_job));
    g_job_valid = 0;
    reset_defaults();
    parse_csv_keywords(args);

    if (resolve_syms() < 0)
        return -1;
    if (install_hooks() < 0)
        return -1;

    g_nr_range = 0;
    pr_info("hide-maps: ready nr_kw=%d maps=%d/%d mf_readdir=%d filldir64=%d "
            "mf_get_link=%d API=prctl(0x%x) +RANGE\n",
            g_nr_kw, g_hooked_vma, g_hooked_smap, g_hooked_mf_readdir,
            g_hooked_filldir64, g_hooked_mf_get_link, PR_HIDEMAPS_PING);
    return 0;
}

long hide_maps_control0(const char *args, char *__user out_msg, int outlen)
{
    char buf[320];
    if (g_job_valid)
        snprintf(buf, sizeof(buf), "kw=%d range=%d job=%s so=%s st=%d",
                 g_nr_kw, g_nr_range, g_job.package, g_job.so_path, g_job.status);
    else
        snprintf(buf, sizeof(buf), "kw=%d range=%d job=none", g_nr_kw, g_nr_range);
    if (args && args[0])
        parse_csv_keywords(args);
    return kpm_demo_copy_message(buf, out_msg, outlen);
}

long hide_maps_exit(void *__user reserved)
{
    (void)reserved;
    remove_hooks();
    return kpm_demo_log_exit("hide-maps");
}

#ifndef STEALTH_FUSED
KPM_INIT(hide_maps_init);
KPM_CTL0(hide_maps_control0);
KPM_EXIT(hide_maps_exit);
#endif
