/*
 * dbc — 一键启动器（纯内核注入，无 ptrace / 无 tinjector）
 *
 * 流程：
 *   1) 写出内嵌的 libdbc.so
 *   2) load stealth.kpm（hide-maps + kload + wxshadow）
 *   3) 强制重启目标包，等 libUE4 映射
 *   4) kload 内核劫持 dlopen 完成注入
 *
 * 用法: ./dbc [--key KEY] [--package PKG]
 */
#define _GNU_SOURCE
#include "stealth_inject.h"
#include "kload_api.h"
#include "so_embed.h"

#include <dirent.h>
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* ---------- 默认配置 ---------- */
static const char *g_package = "com.tencent.letsgo";
/* 本机实测 launcher：com.epicgames.ue4.GameActivityExt（monkey 在部分机型拉不起来） */
static const char *g_activity = "com.epicgames.ue4.GameActivityExt";
static const char *g_wait_lib = "libUE4.so";
static const char *g_key = "Lkdj4K46iwowb3K+-+@+++";
static const char *g_kpatch = "/data/local/tmp/kpatch";
static const char *g_kpm = "/data/local/tmp/stealth.kpm";
static const char *g_log_path = "/data/local/tmp/dbc.log";
static int g_timeout_sec = 90;
static int g_delay_ms = 800;
static int g_force_restart = 1; /* 默认强制重启，确保会有 dlopen 被劫持 */

/* 内嵌 so 落盘路径 */
static char g_so_path[256];
static FILE *g_log_fp;

/* 同时打 stdout + 文件（adb 下也能 cat 到日志） */
static void log_msg(const char *fmt, ...) {
    va_list ap;
    va_list ap2;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    vprintf(fmt, ap);
    fflush(stdout);
    if (g_log_fp) {
        vfprintf(g_log_fp, fmt, ap2);
        fflush(g_log_fp);
    }
    va_end(ap2);
    va_end(ap);
}

static void log_err(const char *fmt, ...) {
    va_list ap;
    va_list ap2;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    vfprintf(stderr, fmt, ap);
    fflush(stderr);
    if (g_log_fp) {
        vfprintf(g_log_fp, fmt, ap2);
        fflush(g_log_fp);
    }
    va_end(ap2);
    va_end(ap);
}

/* ---------- 内核模块就绪 ---------- */
static int kpm_hide_ready(void) {
    return prctl(PR_HIDEMAPS_PING, 0, 0, 0, 0) == HIDEMAPS_MAGIC;
}

static int kpm_kload_ready(void) {
    return prctl(PR_KLOAD_PING, 0, 0, 0, 0) == KLOAD_MAGIC;
}

/* wxshadow ping：与 kpm 约定 */
static int kpm_wx_ready(void) {
    return prctl(0x57580005, 0, 0, 0, 0) >= 0;
}

static int kpm_load(const char *path) {
    char cmd[640];
    if (access(g_kpatch, X_OK) != 0) {
        fprintf(stderr, "[-] kpatch 不可执行: %s errno=%d\n", g_kpatch, errno);
        return -1;
    }
    if (access(path, R_OK) != 0) {
        fprintf(stderr, "[-] kpm 不可读: %s errno=%d\n", path, errno);
        return -1;
    }
    snprintf(cmd, sizeof(cmd), "%s '%s' kpm load %s 2>/dev/null", g_kpatch, g_key, path);
    printf("[*] 加载 kpm: %s\n", path);
    system(cmd);
    usleep(300 * 1000);
    return 0;
}

static int ensure_kpm(void) {
    if ((!kpm_hide_ready() || !kpm_kload_ready() || !kpm_wx_ready()) && g_key[0]) {
        printf("[*] 内核模块未齐，尝试加载 stealth.kpm …\n");
        kpm_load(g_kpm);
    }
    printf("[*] 模块状态 hide=%d kload=%d wx=%d\n", kpm_hide_ready(), kpm_kload_ready(),
           kpm_wx_ready());
    if (!kpm_hide_ready() || !kpm_kload_ready() || !kpm_wx_ready()) {
        fprintf(stderr, "[-] 内核模块未就绪（需要 stealth.kpm + kpatch + 正确密钥）\n");
        return -1;
    }
    printf("[+] 内核就绪：hide-maps + kload + wxshadow\n");
    return 0;
}

/* ---------- 释放内嵌 so ---------- */
static int dump_embedded_so(void) {
    unsigned r = 0;
    int fd;
    size_t n;

    if (dbc_so_len == 0) {
        fprintf(stderr, "[-] 未内嵌 so（请完整编译 dbc）\n");
        return -1;
    }

    fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        (void)read(fd, &r, sizeof(r));
        close(fd);
    } else {
        r = (unsigned)time(NULL) ^ (unsigned)getpid();
    }

    snprintf(g_so_path, sizeof(g_so_path), "/data/local/tmp/.d%08x.so", r);
    fd = open(g_so_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0755);
    if (fd < 0) {
        fprintf(stderr, "[-] 无法写出 so: %s errno=%d\n", g_so_path, errno);
        return -1;
    }
    n = 0;
    while (n < dbc_so_len) {
        ssize_t w = write(fd, dbc_so + n, dbc_so_len - n);
        if (w <= 0) {
            close(fd);
            unlink(g_so_path);
            fprintf(stderr, "[-] 写 so 中断 n=%zu errno=%d\n", n, errno);
            return -1;
        }
        n += (size_t)w;
    }
    close(fd);
    chmod(g_so_path, 0755);
    printf("[+] 已释放内嵌模块 → %s (%u 字节)\n", g_so_path, (unsigned)dbc_so_len);
    return 0;
}

/* ---------- 进程 / 包 ---------- */
static int cmdline_matches_package(pid_t pid, const char *pkg) {
    char path[64], buf[512];
    int fd, n, i;

    snprintf(path, sizeof(path), "/proc/%d/cmdline", (int)pid);
    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return 0;
    n = (int)read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0)
        return 0;
    buf[n] = 0;
    if (strcmp(buf, pkg) == 0)
        return 1;
    for (i = 0; i < n - 1; i++) {
        if (buf[i] == 0 && strcmp(buf + i + 1, pkg) == 0)
            return 1;
    }
    return strstr(buf, pkg) != NULL;
}

/*
 * 找模块基址：
 *  1) /proc/pid/maps（可能被 hide-maps 关键字过滤，.so 行会消失）
 *  2) /proc/pid/map_files + readlink（hide 后仍完整，推荐）
 */
static int find_module_base(pid_t pid, const char *substr, unsigned long *out_base, char *out_path,
                            size_t out_path_len) {
    char maps_path[64], line[768];
    char dir[96], full[200], link[512];
    FILE *fp;
    DIR *d;
    struct dirent *ent;

    *out_base = 0;

    /* --- maps --- */
    snprintf(maps_path, sizeof(maps_path), "/proc/%d/maps", (int)pid);
    fp = fopen(maps_path, "r");
    if (fp) {
        while (fgets(line, sizeof(line), fp)) {
            unsigned long s = 0, e = 0;
            char perms[8] = {0};
            char *path;

            if (sscanf(line, "%lx-%lx %7s", &s, &e, perms) < 3)
                continue;
            path = strrchr(line, ' ');
            if (!path)
                continue;
            path++;
            {
                size_t L = strlen(path);
                while (L && (path[L - 1] == '\n' || path[L - 1] == '\r'))
                    path[--L] = 0;
            }
            if (!path[0] || path[0] == '[')
                continue;
            if (!strstr(path, substr))
                continue;
            if (*out_base == 0 || s < *out_base) {
                *out_base = s;
                if (out_path && out_path_len) {
                    strncpy(out_path, path, out_path_len - 1);
                    out_path[out_path_len - 1] = 0;
                }
            }
        }
        fclose(fp);
        if (*out_base)
            return 0;
    }

    /* --- map_files（hide-maps 后仍可见）--- */
    snprintf(dir, sizeof(dir), "/proc/%d/map_files", (int)pid);
    d = opendir(dir);
    if (!d)
        return -1;
    while ((ent = readdir(d))) {
        unsigned long s = 0, e = 0;
        ssize_t ln;
        if (ent->d_name[0] == '.')
            continue;
        if (sscanf(ent->d_name, "%lx-%lx", &s, &e) != 2 || e <= s)
            continue;
        snprintf(full, sizeof(full), "%s/%s", dir, ent->d_name);
        ln = readlink(full, link, sizeof(link) - 1);
        if (ln < 0)
            continue;
        link[ln] = 0;
        if (!strstr(link, substr))
            continue;
        if (*out_base == 0 || s < *out_base) {
            *out_base = s;
            if (out_path && out_path_len) {
                strncpy(out_path, link, out_path_len - 1);
                out_path[out_path_len - 1] = 0;
            }
        }
    }
    closedir(d);
    return *out_base ? 0 : -1;
}

static pid_t find_package_pid(const char *pkg) {
    DIR *d = opendir("/proc");
    struct dirent *ent;
    pid_t found = 0;
    char cmd[160], buf[64];
    FILE *fp;

    /* 优先 pidof（快） */
    snprintf(cmd, sizeof(cmd), "pidof %s 2>/dev/null", pkg);
    fp = popen(cmd, "r");
    if (fp) {
        if (fgets(buf, sizeof(buf), fp)) {
            char *end = NULL;
            long p = strtol(buf, &end, 10);
            if (p > 1)
                found = (pid_t)p;
        }
        pclose(fp);
        if (found > 0)
            return found;
    }

    if (!d)
        return 0;
    while ((ent = readdir(d))) {
        char *end = NULL;
        pid_t pid;
        if (ent->d_name[0] < '1' || ent->d_name[0] > '9')
            continue;
        pid = (pid_t)strtol(ent->d_name, &end, 10);
        if (!end || *end || pid <= 1)
            continue;
        if (cmdline_matches_package(pid, pkg)) {
            found = pid;
            break;
        }
    }
    closedir(d);
    return found;
}

static int process_alive(pid_t pid) {
    char p[64];
    snprintf(p, sizeof(p), "/proc/%d", (int)pid);
    return access(p, F_OK) == 0;
}

/* 解析 launcher Activity：优先 --activity，否则 dumpsys/resolve */
static int resolve_launcher_component(const char *pkg, char *out, size_t out_len) {
    char cmd[320], line[512];
    FILE *fp;

    if (g_activity && g_activity[0]) {
        snprintf(out, out_len, "%s/%s", pkg, g_activity);
        return 0;
    }

    snprintf(cmd, sizeof(cmd),
             "cmd package resolve-activity --brief %s 2>/dev/null | tail -n 1", pkg);
    fp = popen(cmd, "r");
    if (fp) {
        if (fgets(line, sizeof(line), fp)) {
            size_t L = strlen(line);
            while (L && (line[L - 1] == '\n' || line[L - 1] == '\r' || line[L - 1] == ' '))
                line[--L] = 0;
            if (strchr(line, '/')) {
                strncpy(out, line, out_len - 1);
                out[out_len - 1] = 0;
                pclose(fp);
                return 0;
            }
        }
        pclose(fp);
    }
    /* 兜底：常见 UE4 扩展 Activity */
    snprintf(out, out_len, "%s/com.epicgames.ue4.GameActivityExt", pkg);
    return 0;
}

/* 强制停包再 am start，保证后续有 dlopen 可劫持 */
static void restart_package(const char *pkg) {
    char cmd[512], component[256];
    int st;

    resolve_launcher_component(pkg, component, sizeof(component));
    log_msg("[*] 强制重启包: %s\n", pkg);
    log_msg("[*] Activity: %s\n", component);

    snprintf(cmd, sizeof(cmd), "am force-stop %s >/dev/null 2>&1", pkg);
    system(cmd);
    usleep(800 * 1000);

    /* 主路径：显式 component（monkey 在部分机型无效） */
    snprintf(cmd, sizeof(cmd), "am start -n %s 2>&1", component);
    log_msg("[*] 执行: %s\n", cmd);
    st = system(cmd);
    log_msg("[*] am start 返回 status=%d\n", st);
    usleep(500 * 1000);

    if (find_package_pid(pkg) <= 0) {
        /* 备用 monkey */
        snprintf(cmd, sizeof(cmd),
                 "monkey -p %s -c android.intent.category.LAUNCHER 1 >/dev/null 2>&1", pkg);
        log_msg("[*] 备用 monkey 启动\n");
        system(cmd);
        usleep(500 * 1000);
    }

    if (find_package_pid(pkg) <= 0) {
        snprintf(cmd, sizeof(cmd), "am start -a android.intent.action.MAIN -c "
                                   "android.intent.category.LAUNCHER %s 2>&1",
                 pkg);
        log_msg("[*] 备用 MAIN/LAUNCHER 启动\n");
        system(cmd);
        usleep(500 * 1000);
    }

    log_msg("[*] 启动后 pid=%d\n", (int)find_package_pid(pkg));
}

/* ---------- ELF / dlopen ---------- */
static long local_symbol_offset(const char *lib_path, const char *sym) {
    int fd;
    struct stat st;
    void *map;
    Elf64_Ehdr *eh;
    Elf64_Shdr *sh;
    const char *shstr;
    Elf64_Sym *dynsym = NULL;
    const char *dynstr = NULL;
    size_t nsym = 0;
    long off = -1;
    int i;

    fd = open(lib_path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    if (fstat(fd, &st) < 0) {
        close(fd);
        return -1;
    }
    map = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (map == MAP_FAILED)
        return -1;

    eh = (Elf64_Ehdr *)map;
    if (memcmp(eh->e_ident, ELFMAG, SELFMAG)) {
        munmap(map, (size_t)st.st_size);
        return -1;
    }
    sh = (Elf64_Shdr *)((char *)map + eh->e_shoff);
    shstr = (const char *)map + sh[eh->e_shstrndx].sh_offset;
    for (i = 0; i < eh->e_shnum; i++) {
        const char *nm = shstr + sh[i].sh_name;
        if (sh[i].sh_type == SHT_DYNSYM) {
            dynsym = (Elf64_Sym *)((char *)map + sh[i].sh_offset);
            nsym = sh[i].sh_size / sizeof(Elf64_Sym);
        }
        if (sh[i].sh_type == SHT_STRTAB && !strcmp(nm, ".dynstr"))
            dynstr = (const char *)map + sh[i].sh_offset;
    }
    if (dynsym && dynstr) {
        size_t j;
        for (j = 0; j < nsym; j++) {
            if (!dynsym[j].st_name)
                continue;
            if (!strcmp(dynstr + dynsym[j].st_name, sym) && dynsym[j].st_value) {
                off = (long)dynsym[j].st_value;
                break;
            }
        }
    }
    munmap(map, (size_t)st.st_size);
    return off;
}

static unsigned long resolve_remote_dlopen(pid_t pid) {
    unsigned long base = 0;
    char path[512] = {0};
    int attempt;
    /* Android 新版本 libdl 里的 dlopen 只是跳板且可能落在不可执行页；
     * 真正实现在 linker64 的 __loader_dlopen。 */
    static const char *k_syms[] = {
        "__loader_dlopen",
        "__dl___loader_dlopen",
        "android_dlopen_ext",
        "dlopen",
        NULL,
    };

    for (attempt = 0; attempt < 40; attempt++) {
        /* 1) linker64 优先 */
        if (find_module_base(pid, "linker64", &base, path, sizeof(path)) == 0 && path[0]) {
            int si;
            for (si = 0; k_syms[si]; si++) {
                long off = local_symbol_offset(path, k_syms[si]);
                if (off > 0) {
                    unsigned long va = base + (unsigned long)off;
                    log_msg("[+] remote dlopen = 0x%lx (linker base=0x%lx off=0x%lx sym=%s)\n", va,
                            base, (unsigned long)off, k_syms[si]);
                    return va;
                }
            }
            /* 磁盘上的真实路径可能是 /apex/.../bin/linker64 */
            {
                long off = local_symbol_offset("/apex/com.android.runtime/bin/linker64",
                                               "__loader_dlopen");
                if (off > 0) {
                    unsigned long va = base + (unsigned long)off;
                    log_msg("[+] remote dlopen = 0x%lx (linker64 offline sym)\n", va);
                    return va;
                }
            }
        }

        /* 2) 回退 libdl（旧设备） */
        if (find_module_base(pid, "libdl.so", &base, path, sizeof(path)) == 0 && path[0]) {
            long off = local_symbol_offset(path, "android_dlopen_ext");
            if (off <= 0)
                off = local_symbol_offset(path, "dlopen");
            if (off > 0) {
                unsigned long va = base + (unsigned long)off;
                log_msg("[+] remote dlopen = 0x%lx (libdl base=0x%lx off=0x%lx)\n", va, base,
                        (unsigned long)off);
                return va;
            }
        }
        usleep(100 * 1000);
    }
    log_err("[-] cannot resolve remote dlopen\n");
    return 0;
}

/* 把 so 路径字符串写入目标进程可写内存 */
static unsigned long write_remote_path(pid_t pid, const char *path_str) {
    char maps_path[64], line[768], verify[384];
    FILE *fp;
    char mem_path[64];
    int fd, nc = 0, i;
    size_t plen = strlen(path_str) + 1;
    unsigned long candidates[24];

    if (plen > sizeof(verify))
        return 0;

    snprintf(maps_path, sizeof(maps_path), "/proc/%d/maps", (int)pid);
    fp = fopen(maps_path, "r");
    if (!fp) {
        fprintf(stderr, "[-] 打开 maps 失败 pid=%d errno=%d\n", (int)pid, errno);
        return 0;
    }
    while (fgets(line, sizeof(line), fp) && nc < 24) {
        unsigned long s, e;
        char perms[8];
        if (sscanf(line, "%lx-%lx %7s", &s, &e, perms) < 3)
            continue;
        if (strncmp(perms, "rw", 2) != 0)
            continue;
        if (strstr(line, "[stack]") || strstr(line, "[vdso]") || strstr(line, "[vvar]"))
            continue;
        if (e - s < 0x2000)
            continue;
        if (s < 0x1000UL)
            continue;
        candidates[nc++] = (s + 0x1000) & ~0xFULL;
    }
    fclose(fp);
    if (!nc) {
        fprintf(stderr, "[-] 未找到可写匿名区写入路径\n");
        return 0;
    }

    snprintf(mem_path, sizeof(mem_path), "/proc/%d/mem", (int)pid);
    fd = open(mem_path, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        struct iovec local_iov, remote_iov;
        for (i = 0; i < nc; i++) {
            local_iov.iov_base = (void *)path_str;
            local_iov.iov_len = plen;
            remote_iov.iov_base = (void *)(uintptr_t)candidates[i];
            remote_iov.iov_len = plen;
            if (process_vm_writev(pid, &local_iov, 1, &remote_iov, 1, 0) != (ssize_t)plen)
                continue;
            local_iov.iov_base = verify;
            if (process_vm_readv(pid, &local_iov, 1, &remote_iov, 1, 0) == (ssize_t)plen &&
                memcmp(verify, path_str, plen) == 0) {
                printf("[+] 远程路径 @0x%lx (process_vm_writev)\n", candidates[i]);
                return candidates[i];
            }
        }
        fprintf(stderr, "[-] process_vm_writev 写入路径失败 errno=%d\n", errno);
        return 0;
    }

    for (i = 0; i < nc; i++) {
        if (lseek(fd, (off_t)candidates[i], SEEK_SET) < 0)
            continue;
        if (write(fd, path_str, plen) != (ssize_t)plen)
            continue;
        if (lseek(fd, (off_t)candidates[i], SEEK_SET) < 0)
            continue;
        memset(verify, 0, sizeof(verify));
        if (read(fd, verify, plen) != (ssize_t)plen)
            continue;
        if (memcmp(verify, path_str, plen) != 0)
            continue;
        close(fd);
        printf("[+] 远程路径 @0x%lx\n", candidates[i]);
        return candidates[i];
    }
    close(fd);
    fprintf(stderr, "[-] /proc/pid/mem 写入路径失败\n");
    return 0;
}

static int get_uid(pid_t pid, uid_t *uid_out) {
    char path[64], line[128];
    FILE *fp;

    snprintf(path, sizeof(path), "/proc/%d/status", (int)pid);
    fp = fopen(path, "r");
    if (!fp)
        return -1;
    while (fgets(line, sizeof(line), fp)) {
        unsigned x = 0;
        if (sscanf(line, "Uid: %u", &x) == 1) {
            *uid_out = (uid_t)x;
            fclose(fp);
            return 0;
        }
    }
    fclose(fp);
    return -1;
}

/* 拷到 app code_cache，随机名；内核 dlopen 用该路径 */
static int stage_so_to_app(pid_t pid, const char *pkg, const char *src, char *out, size_t out_len) {
    uid_t uid = 0;
    char cmd[1280], dir[256], name[40];
    unsigned r = 0;
    int fd, st;

    if (get_uid(pid, &uid) < 0) {
        fprintf(stderr, "[-] 取 uid 失败 pid=%d\n", (int)pid);
        return -1;
    }
    fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        (void)read(fd, &r, sizeof(r));
        close(fd);
    } else {
        r = (unsigned)time(NULL) ^ (unsigned)pid;
    }
    snprintf(name, sizeof(name), "l%08x.so", r);
    snprintf(dir, sizeof(dir), "/data/data/%s/code_cache", pkg);
    snprintf(out, out_len, "%s/%s", dir, name);

    /* 优先 code_cache；失败再试 /data/user/0 */
    snprintf(cmd, sizeof(cmd),
             "mkdir -p '%s' 2>/dev/null; cp -f '%s' '%s' && chown %u:%u '%s' && chmod 755 '%s' && "
             "test -f '%s'",
             dir, src, out, (unsigned)uid, (unsigned)uid, out, out, out);
    printf("[*] 暂存模块 → %s (uid=%u)\n", out, (unsigned)uid);
    st = system(cmd);
    if (!(WIFEXITED(st) && WEXITSTATUS(st) == 0)) {
        snprintf(dir, sizeof(dir), "/data/user/0/%s/code_cache", pkg);
        snprintf(out, out_len, "%s/%s", dir, name);
        snprintf(cmd, sizeof(cmd),
                 "mkdir -p '%s' 2>/dev/null; cp -f '%s' '%s' && chown %u:%u '%s' && chmod 755 '%s' "
                 "&& test -f '%s'",
                 dir, src, out, (unsigned)uid, (unsigned)uid, out, out, out);
        printf("[*] 重试暂存 → %s\n", out);
        st = system(cmd);
        if (!(WIFEXITED(st) && WEXITSTATUS(st) == 0)) {
            fprintf(stderr, "[-] 暂存 so 失败 status=%d\n", st);
            out[0] = 0;
            return -1;
        }
    }

    /* 只藏本次暂存文件名/路径，不要用 ".so" 这种过宽关键字 */
    prctl(PR_HIDEMAPS_ADD_KW, (unsigned long)out, 0, 0, 0);
    prctl(PR_HIDEMAPS_ADD_KW, (unsigned long)name, 0, 0, 0);
    return 0;
}

static int register_hide_ranges(pid_t pid, const char *basename) {
    char dir[128], link[512];
    DIR *d;
    struct dirent *ent;
    int n = 0;

    snprintf(dir, sizeof(dir), "/proc/%d/map_files", (int)pid);
    d = opendir(dir);
    if (!d)
        return 0;
    while ((ent = readdir(d))) {
        unsigned long s = 0, ee = 0;
        char full[256];
        ssize_t ln;
        if (ent->d_name[0] == '.')
            continue;
        if (sscanf(ent->d_name, "%lx-%lx", &s, &ee) != 2 || ee <= s)
            continue;
        snprintf(full, sizeof(full), "%s/%s", dir, ent->d_name);
        ln = readlink(full, link, sizeof(link) - 1);
        if (ln < 0)
            continue;
        link[ln] = 0;
        if (!strstr(link, basename))
            continue;
        if (prctl(PR_HIDEMAPS_ADD_RANGE, s, ee, 0, 0) == 0) {
            printf("[+] 隐藏区间 [%lx,%lx)\n", s, ee);
            n++;
        }
    }
    closedir(d);
    return n;
}

/* 内核 kload 注入 */
static int kernel_inject(pid_t pid, const char *pkg, const char *so_file) {
    unsigned long dlopen_va, path_uaddr;
    char staged[320];
    int st, i;
    const char *base_name;

    if (stage_so_to_app(pid, pkg, so_file, staged, sizeof(staged)) < 0)
        return -1;

    dlopen_va = resolve_remote_dlopen(pid);
    if (!dlopen_va)
        return -1;

    path_uaddr = write_remote_path(pid, staged);
    if (!path_uaddr) {
        fprintf(stderr, "[-] 写入远程路径失败\n");
        return -1;
    }

    errno = 0;
    if (prctl(PR_KLOAD_INJECT, (unsigned long)pid, path_uaddr, dlopen_va, 0) != 0) {
        fprintf(stderr, "[-] kload INJECT 失败 errno=%d\n", errno);
        return -1;
    }
    printf("[*] kload 已武装 pid=%d path@0x%lx dlopen@0x%lx\n", (int)pid, path_uaddr, dlopen_va);
    printf("[*] 等待内核劫持下一次 dlopen …\n");

    for (i = 0; i < 200; i++) {
        st = (int)prctl(PR_KLOAD_STATUS, 0, 0, 0, 0);
        if (st == 2) {
            printf("[+] kload 劫持完成\n");
            usleep(900 * 1000);
            if (!process_alive(pid)) {
                fprintf(stderr, "[-] 注入后进程已死（so 构造函数可能崩了）\n");
                return -1;
            }
            base_name = strrchr(staged, '/');
            base_name = base_name ? base_name + 1 : staged;
            usleep(400 * 1000);
            printf("[*] 登记隐藏: %d 段\n", register_hide_ranges(pid, base_name));
            unlink(staged);
            usleep(100 * 1000);
            register_hide_ranges(pid, base_name);
            /* 再确认 maps 里是否有我们的 so 痕迹或 DbcHK 日志靠用户看 */
            return 0;
        }
        if (st < 0) {
            fprintf(stderr, "[-] kload 错误 status=%d\n", st);
            return -1;
        }
        if (!process_alive(pid)) {
            fprintf(stderr, "[-] 等待劫持时进程已死\n");
            return -1;
        }
        /* 进程空闲时可能长期无 dlopen：主动轻推一次 activity */
        if (i == 40 || i == 80 || i == 120) {
            char cmd[400], component[256];
            resolve_launcher_component(pkg, component, sizeof(component));
            snprintf(cmd, sizeof(cmd), "am start -n %s >/dev/null 2>&1", component);
            system(cmd);
            log_msg("[*] 触发 activity 以产生 dlopen (tick=%d status=%d)\n", i, st);
        }
        usleep(50 * 1000);
    }
    fprintf(stderr, "[-] kload 超时（status 一直未到 2）\n");
    return -1;
}

static void print_usage(const char *argv0) {
    fprintf(stderr,
            "Usage: %s [options]\n"
            "  --key KEY\n"
            "  --package PKG          default com.tencent.letsgo\n"
            "  --activity CLS         default com.epicgames.ue4.GameActivityExt\n"
            "  --kpm PATH\n"
            "  --kpatch PATH\n"
            "  --delay MS\n"
            "  --timeout SEC\n"
            "  --no-restart\n"
            "  --wait-lib NAME        empty to skip wait\n"
            "  --log PATH             default /data/local/tmp/dbc.log\n",
            argv0);
}

int main(int argc, char **argv) {
    pid_t pid = 0;
    time_t t0;
    int i;
    int last_report = -1;

    /* adb su 下无 tty，必须无缓冲 + 写文件，否则像“没日志” */
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--key") && i + 1 < argc)
            g_key = argv[++i];
        else if (!strcmp(argv[i], "--package") && i + 1 < argc)
            g_package = argv[++i];
        else if (!strcmp(argv[i], "--activity") && i + 1 < argc)
            g_activity = argv[++i];
        else if (!strcmp(argv[i], "--kpm") && i + 1 < argc)
            g_kpm = argv[++i];
        else if (!strcmp(argv[i], "--kpatch") && i + 1 < argc)
            g_kpatch = argv[++i];
        else if (!strcmp(argv[i], "--delay") && i + 1 < argc)
            g_delay_ms = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--timeout") && i + 1 < argc)
            g_timeout_sec = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--wait-lib") && i + 1 < argc)
            g_wait_lib = argv[++i];
        else if (!strcmp(argv[i], "--log") && i + 1 < argc)
            g_log_path = argv[++i];
        else if (!strcmp(argv[i], "--no-restart"))
            g_force_restart = 0;
        else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            print_usage(argv[0]);
            return 0;
        } else {
            print_usage(argv[0]);
            return 1;
        }
    }

    g_log_fp = fopen(g_log_path, "w");
    if (!g_log_fp)
        fprintf(stderr, "[!] cannot open log %s errno=%d\n", g_log_path, errno);

    log_msg("========== dbc kernel inject ==========\n");
    log_msg("  package : %s\n", g_package);
    log_msg("  activity: %s\n", g_activity && g_activity[0] ? g_activity : "(auto)");
    log_msg("  wait_lib: %s\n", g_wait_lib[0] ? g_wait_lib : "(none)");
    log_msg("  restart : %s\n", g_force_restart ? "yes" : "no");
    log_msg("  log     : %s\n", g_log_path);

    if (dump_embedded_so() != 0) {
        if (g_log_fp)
            fclose(g_log_fp);
        return 2;
    }
    /* dump uses printf - mirror key lines */
    log_msg("[+] embedded so ready: %s (%u bytes)\n", g_so_path, (unsigned)dbc_so_len);

    if (ensure_kpm() != 0) {
        log_err("[-] kpm not ready\n");
        if (g_log_fp)
            fclose(g_log_fp);
        return 3;
    }
    log_msg("[+] kpm ready hide=%d kload=%d wx=%d\n", kpm_hide_ready(), kpm_kload_ready(),
            kpm_wx_ready());

    /*
     * 注意：绝不能 ADD_KW ".so" —— 会把 maps 里所有 so 都藏掉，
     * 导致永远找不到 libdl/libUE4，注入前就绪检测失败。
     * 隐藏只在注入成功后按具体 basename 登记 range。
     * 先 CLEAR 清掉历史过宽关键字（含上次误加的 ".so"）。
     */
    prctl(PR_HIDEMAPS_CLEAR, 0, 0, 0, 0);
    prctl(PR_HIDEMAPS_CLR_RANGE, 0, 0, 0, 0);

    if (g_force_restart) {
        restart_package(g_package);
    } else if (find_package_pid(g_package) <= 0) {
        restart_package(g_package);
    } else {
        log_msg("[*] target already running (--no-restart)\n");
    }

    t0 = time(NULL);
    for (;;) {
        unsigned long b = 0;
        char p[256] = {0};
        int elapsed = (int)(time(NULL) - t0);
        int has_dl = 0;
        pid = find_package_pid(g_package);

        /* 新 Android 可能无独立 libdl.so，linker64 也算就绪 */
        if (pid > 0) {
            if (find_module_base(pid, "libdl.so", &b, p, sizeof(p)) == 0)
                has_dl = 1;
            else if (find_module_base(pid, "linker64", &b, p, sizeof(p)) == 0)
                has_dl = 1;
            else if (find_module_base(pid, "/bin/linker", &b, p, sizeof(p)) == 0)
                has_dl = 1;
        }

        if (pid > 0 && has_dl) {
            unsigned long ub = 0;
            if (!g_wait_lib[0] || find_module_base(pid, g_wait_lib, &ub, NULL, 0) == 0) {
                log_msg("[+] process ready pid=%d dl=%s\n", (int)pid, p);
                if (g_wait_lib[0])
                    log_msg("[+] mapped %s @0x%lx\n", g_wait_lib, ub);
                break;
            }
            if (elapsed != last_report) {
                log_msg("[*] wait %s ... pid=%d t=%ds\n", g_wait_lib, (int)pid, elapsed);
                last_report = elapsed;
            }
        } else if (pid <= 0) {
            if (elapsed != last_report) {
                log_msg("[*] wait process start ... t=%ds\n", elapsed);
                last_report = elapsed;
            }
            if (elapsed > 0 && (elapsed % 5) == 0)
                restart_package(g_package);
        } else if (elapsed != last_report) {
            log_msg("[*] wait linker/libdl ... pid=%d t=%ds\n", (int)pid, elapsed);
            last_report = elapsed;
        }
        if (time(NULL) - t0 >= g_timeout_sec) {
            log_err("[-] wait process timeout (%ds). package=%s\n", g_timeout_sec, g_package);
            log_err("    try: am start -n %s/%s\n", g_package,
                    g_activity && g_activity[0] ? g_activity : "com.epicgames.ue4.GameActivityExt");
            unlink(g_so_path);
            if (g_log_fp)
                fclose(g_log_fp);
            return 4;
        }
        usleep(200 * 1000);
    }

    if (g_delay_ms > 0) {
        log_msg("[*] delay %d ms before inject\n", g_delay_ms);
        usleep((useconds_t)g_delay_ms * 1000);
    }

    if (!process_alive(pid)) {
        log_err("[-] process died before inject\n");
        unlink(g_so_path);
        if (g_log_fp)
            fclose(g_log_fp);
        return 5;
    }

    if (kernel_inject(pid, g_package, g_so_path) != 0) {
        log_err("[-] kernel inject failed\n");
        unlink(g_so_path);
        if (g_log_fp)
            fclose(g_log_fp);
        return 6;
    }

    unlink(g_so_path);
    log_msg("[+] done: module injected by kload\n");
    log_msg("    logcat -s DbcHK\n");
    log_msg("    also: cat %s\n", g_log_path);
    if (g_log_fp)
        fclose(g_log_fp);
    return 0;
}
