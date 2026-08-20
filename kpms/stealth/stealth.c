/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * stealth.kpm — 融合 hide-maps + kload + wxshadow + dbc-rw 为单一 KPM
 *
 * 加载一次即可：
 *   - hide-maps: maps/smaps/map_files 隐藏 + prctl
 *   - kload:     无 ptrace dlopen 劫持
 *   - wxshadow:  W^X shadow BP/PATCH + prctl
 *   - dbc-rw:    跨进程内存读写（syscall 41 + magic）
 *
 * Usage:
 *   kpatch <key> kpm load /data/local/tmp/stealth.kpm
 *   # 读写见 tools/dbc_rw/
 */

#include <compiler.h>
#include <kpmodule.h>
#include <linux/printk.h>
#include <linux/string.h>
#include <kputils.h>
#include "../common/kpm_demo_helpers.h"

KPM_MODULE_INFO("stealth",
                "1.1.0",
                "GPL v2",
                "dbc",
                "Unified: hide-maps + kload + wxshadow + dbc-rw");

/* 子模块导出（STEALTH_FUSED） */
long hide_maps_init(const char *args, const char *event, void *__user reserved);
long hide_maps_exit(void *__user reserved);
long hide_maps_control0(const char *args, char *__user out_msg, int outlen);

long kload_init(const char *args, const char *event, void *__user reserved);
long kload_exit(void *__user reserved);
long kload_control0(const char *args, char *__user out_msg, int outlen);

long wxshadow_init(const char *args, const char *event, void *__user reserved);
long wxshadow_exit(void *__user reserved);
long wxshadow_control(const char *args, char *__user out_msg, int outlen);

long dbc_rw_init(const char *args, const char *event, void *__user reserved);
long dbc_rw_exit(void *__user reserved);
long dbc_rw_control0(const char *args, char *__user out_msg, int outlen);

static int g_hm_ok;
static int g_kl_ok;
static int g_wx_ok;
static int g_rw_ok;

/*
 * 初始化顺序：
 *   1) hide-maps
 *   2) kload
 *   3) wxshadow
 *   4) dbc-rw   — syscall(41) 读写，失败不回滚整栈（可选组件）
 */
static long stealth_init(const char *args, const char *event, void *__user reserved)
{
    long rc;

    pr_info("stealth: init begin (hm+kload+wx+rw), args=%s\n",
            args ? args : "(null)");

    g_hm_ok = g_kl_ok = g_wx_ok = g_rw_ok = 0;

    rc = hide_maps_init(args, event, reserved);
    if (rc) {
        pr_err("stealth: hide-maps init failed rc=%ld\n", rc);
        return rc;
    }
    g_hm_ok = 1;
    pr_info("stealth: [1/4] hide-maps ready\n");

    rc = kload_init(args, event, reserved);
    if (rc) {
        pr_err("stealth: kload init failed rc=%ld\n", rc);
        hide_maps_exit(reserved);
        g_hm_ok = 0;
        return rc;
    }
    g_kl_ok = 1;
    pr_info("stealth: [2/4] kload ready\n");

    rc = wxshadow_init(args, event, reserved);
    if (rc) {
        pr_err("stealth: wxshadow init failed rc=%ld\n", rc);
        kload_exit(reserved);
        hide_maps_exit(reserved);
        g_kl_ok = g_hm_ok = 0;
        return rc;
    }
    g_wx_ok = 1;
    pr_info("stealth: [3/4] wxshadow ready\n");

    rc = dbc_rw_init(args, event, reserved);
    if (rc) {
        /* 读写失败不拆整栈：注入/隐藏仍可用 */
        pr_warn("stealth: dbc-rw init failed rc=%ld (continue without r/w)\n", rc);
        g_rw_ok = 0;
    } else {
        g_rw_ok = 1;
        pr_info("stealth: [4/4] dbc-rw ready (syscall 41)\n");
    }

    pr_info("stealth: all up hm=%d kl=%d wx=%d rw=%d\n",
            g_hm_ok, g_kl_ok, g_wx_ok, g_rw_ok);
    return 0;
}

static long stealth_exit(void *__user reserved)
{
    pr_info("stealth: exit (reverse order)\n");
    if (g_rw_ok) {
        dbc_rw_exit(reserved);
        g_rw_ok = 0;
    }
    if (g_wx_ok) {
        wxshadow_exit(reserved);
        g_wx_ok = 0;
    }
    if (g_kl_ok) {
        kload_exit(reserved);
        g_kl_ok = 0;
    }
    if (g_hm_ok) {
        hide_maps_exit(reserved);
        g_hm_ok = 0;
    }
    return kpm_demo_log_exit("stealth");
}

/*
 * ctl0：
 *   hide ... | kload ... | wx ... | rw ... | 空=状态
 */
static long stealth_control0(const char *args, char *__user out_msg, int outlen)
{
    char buf[384];

    if (args && !strncmp(args, "hide", 4))
        return hide_maps_control0(args + 4 + (args[4] == ':' || args[4] == ' '),
                                  out_msg, outlen);
    if (args && !strncmp(args, "kload", 5))
        return kload_control0(args + 5 + (args[5] == ':' || args[5] == ' '),
                              out_msg, outlen);
    if (args && (!strncmp(args, "wx", 2) || !strncmp(args, "wxshadow", 8))) {
        const char *p = args;
        if (!strncmp(p, "wxshadow", 8))
            p += 8;
        else
            p += 2;
        if (*p == ':' || *p == ' ')
            p++;
        return wxshadow_control(p, out_msg, outlen);
    }
    if (args && (!strncmp(args, "rw", 2) || !strncmp(args, "dbc", 3))) {
        const char *p = args;
        if (!strncmp(p, "dbc-rw", 6))
            p += 6;
        else if (!strncmp(p, "dbc", 3))
            p += 3;
        else
            p += 2;
        if (*p == ':' || *p == ' ')
            p++;
        return dbc_rw_control0(p, out_msg, outlen);
    }

    snprintf(buf, sizeof(buf),
             "stealth hm=%d kl=%d wx=%d rw=%d | ctl: hide|kload|wx|rw",
             g_hm_ok, g_kl_ok, g_wx_ok, g_rw_ok);
    return kpm_demo_copy_message(buf, out_msg, outlen);
}

KPM_INIT(stealth_init);
KPM_CTL0(stealth_control0);
KPM_EXIT(stealth_exit);
