/*
 * stealth_inject — dbc_lostgo_hook 一体注入器
 *
 * 优先 stealth.kpm（hide-maps + kload + wxshadow + dbc-rw）
 * 内核 kload 无 ptrace 注入；失败回退 tinjector
 *
 * 默认（可改）：
 *   -p com.tencent.letsgo
 *   -s <与本程序同目录或 /data/local/tmp/libdbc.so>
 *   --wait-lib libUE4.so   （等游戏引擎映射后再注，避免过早崩溃）
 */
#define _GNU_SOURCE
#include "stealth_inject.h"
#include "kload_api.h"

#include <elf.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <link.h>
#include <limits.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <time.h>
#include <stdint.h>

/* ---------- 可配置路径（相对本程序 / 默认 tmp） ---------- */
static char g_self_dir[PATH_MAX];
static char g_path_kpatch[PATH_MAX];
static char g_path_stealth[PATH_MAX];
static char g_path_hide[PATH_MAX];
static char g_path_kload[PATH_MAX];
static char g_path_wx[PATH_MAX];
static char g_path_tinject[PATH_MAX];
static char g_path_so_default[PATH_MAX];

static const char *g_pkg = "com.tencent.letsgo";
static const char *g_so; /* 解析后绝对路径 */
static char g_so_buf[PATH_MAX];
static const char *g_key = ""; /* pass --key <superkey> */
static const char *g_wait_lib = "libUE4.so"; /* 空串 = 不等 */
static int g_do_start;
static int g_timeout_sec = 120;
static int g_poll_us = 3000;
static int g_force_tinject;
static int g_delay_ms = 500; /* 就绪后延时再注入 */
static int g_no_tinject;     /* 禁止回退 tinjector */

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage: %s [options]\n"
            "\n"
            "  -p, --package <pkg>   包名 (默认 com.tencent.letsgo)\n"
            "  -s, --so <path>       so 路径 (默认: 同目录 libdbc.so 或 /data/local/tmp/libdbc.so)\n"
            "  --start               强停并拉起后再注入\n"
            "  --wait-lib <name>     等待 maps 出现该库再注入 (默认 libUE4.so；空=只等 libdl)\n"
            "  --delay <ms>          就绪后再等 ms 再注入 (默认 500)\n"
            "  --key <superkey>      自动 load stealth.kpm\n"
            "  --kpm <path>          stealth.kpm 路径\n"
            "  --kpatch <path>       kpatch 路径\n"
            "  --tinject             强制 tinjector\n"
            "  --no-tinject          kload 失败不回退\n"
            "  --timeout <sec>\n"
            "\n"
            "Example:\n"
            "  %s --start\n"
            "  %s -s /data/local/tmp/libdbc.so --wait-lib libUE4.so\n",
            prog, prog, prog);
}

/* 本程序所在目录 */
static void resolve_self_dir(const char *argv0)
{
    char link[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", link, sizeof(link) - 1);
    if (n > 0) {
        link[n] = 0;
        char *slash = strrchr(link, '/');
        if (slash) {
            *slash = 0;
            snprintf(g_self_dir, sizeof(g_self_dir), "%s", link);
            return;
        }
    }
    if (argv0 && argv0[0] == '/') {
        snprintf(g_self_dir, sizeof(g_self_dir), "%s", argv0);
        char *slash = strrchr(g_self_dir, '/');
        if (slash)
            *slash = 0;
        else
            snprintf(g_self_dir, sizeof(g_self_dir), ".");
    } else {
        snprintf(g_self_dir, sizeof(g_self_dir), "/data/local/tmp");
    }
}

static void path_join(char *out, size_t n, const char *dir, const char *name)
{
    snprintf(out, n, "%s/%s", dir, name);
}

static int file_ok(const char *p)
{
    return p && p[0] && access(p, R_OK) == 0;
}

static void init_default_paths(void)
{
    const char *tmp = "/data/local/tmp";
    /* 优先同目录，其次 /data/local/tmp */
    path_join(g_path_kpatch, sizeof(g_path_kpatch), g_self_dir, "kpatch");
    if (!file_ok(g_path_kpatch))
        path_join(g_path_kpatch, sizeof(g_path_kpatch), tmp, "kpatch");

    path_join(g_path_stealth, sizeof(g_path_stealth), g_self_dir, "stealth.kpm");
    if (!file_ok(g_path_stealth))
        path_join(g_path_stealth, sizeof(g_path_stealth), tmp, "stealth.kpm");

    path_join(g_path_hide, sizeof(g_path_hide), tmp, "hide-maps.kpm");
    path_join(g_path_kload, sizeof(g_path_kload), tmp, "kload.kpm");
    path_join(g_path_wx, sizeof(g_path_wx), tmp, "wxshadow.kpm");

    path_join(g_path_tinject, sizeof(g_path_tinject), g_self_dir, "tinjector");
    if (access(g_path_tinject, X_OK) != 0)
        path_join(g_path_tinject, sizeof(g_path_tinject), tmp, "tinjector");

    path_join(g_path_so_default, sizeof(g_path_so_default), g_self_dir, "libdbc.so");
    if (!file_ok(g_path_so_default))
        path_join(g_path_so_default, sizeof(g_path_so_default), tmp, "libdbc.so");
}

/* ---------- hide-maps ---------- */
static int hide_ping(void)
{
    return prctl(PR_HIDEMAPS_PING, 0, 0, 0, 0) == HIDEMAPS_MAGIC;
}
static int hide_add_kw(const char *kw)
{
    return (int)prctl(PR_HIDEMAPS_ADD_KW, (unsigned long)kw, 0, 0, 0);
}
static int hide_add_range(unsigned long start, unsigned long end)
{
    return (int)prctl(PR_HIDEMAPS_ADD_RANGE, start, end, 0, 0);
}
static int hide_register(const char *pkg, const char *so, int flags)
{
    return (int)prctl(PR_HIDEMAPS_REGISTER, (unsigned long)pkg, (unsigned long)so,
                      (unsigned long)flags, 0);
}

static int hide_register_so_ranges(pid_t pid, const char *basename)
{
    char path[128], link[512], line[512], cmd[192];
    DIR *d;
    struct dirent *e;
    FILE *fp;
    int n = 0;

    if (!basename || !basename[0])
        return 0;

    /* 1) map_files 目录项：name=start-end，readlink 看路径 */
    snprintf(path, sizeof(path), "/proc/%d/map_files", (int)pid);
    d = opendir(path);
    if (d) {
        while ((e = readdir(d))) {
            unsigned long s = 0, eaddr = 0;
            char full[192];
            ssize_t ln;
            if (e->d_name[0] == '.')
                continue;
            if (sscanf(e->d_name, "%lx-%lx", &s, &eaddr) != 2 || eaddr <= s)
                continue;
            snprintf(full, sizeof(full), "%s/%s", path, e->d_name);
            ln = readlink(full, link, sizeof(link) - 1);
            if (ln < 0)
                continue;
            link[ln] = 0;
            if (!strstr(link, basename))
                continue;
            if (hide_add_range(s, eaddr) == 0) {
                printf("[+] hide range map_files [%lx,%lx)\n", s, eaddr);
                n++;
            }
        }
        closedir(d);
    }

    /* 2) maps 兜底（可能被 kw 滤掉） */
    snprintf(cmd, sizeof(cmd), "cat /proc/%d/maps 2>/dev/null", (int)pid);
    fp = popen(cmd, "r");
    if (fp) {
        while (fgets(line, sizeof(line), fp)) {
            unsigned long s = 0, eaddr = 0;
            if (!strstr(line, basename))
                continue;
            if (sscanf(line, "%lx-%lx", &s, &eaddr) == 2 && eaddr > s) {
                if (hide_add_range(s, eaddr) == 0) {
                    printf("[+] hide range maps [%lx,%lx)\n", s, eaddr);
                    n++;
                }
            }
        }
        pclose(fp);
    }
    return n;
}

/* ---------- kload ---------- */
static int kload_ping(void)
{
    return prctl(PR_KLOAD_PING, 0, 0, 0, 0) == KLOAD_MAGIC;
}
static int kload_inject(pid_t pid, unsigned long path_uaddr, unsigned long dlopen_va)
{
    return (int)prctl(PR_KLOAD_INJECT, (unsigned long)pid, path_uaddr, dlopen_va, 0);
}
static int kload_status(void)
{
    return (int)prctl(PR_KLOAD_STATUS, 0, 0, 0, 0);
}

static unsigned long write_path_remote(pid_t pid, const char *path)
{
    char cmd[96], line[512];
    FILE *fp;
    char mempath[64];
    char verify[320];
    int fd, n;
    size_t plen = strlen(path) + 1;
    unsigned long candidates[16];
    int nc = 0, i;

    if (plen > sizeof(verify))
        return 0;

    snprintf(cmd, sizeof(cmd), "cat /proc/%d/maps 2>/dev/null", (int)pid);
    fp = popen(cmd, "r");
    if (!fp)
        return 0;
    while (fgets(line, sizeof(line), fp) && nc < 16) {
        unsigned long s, e;
        char perms[8];
        if (sscanf(line, "%lx-%lx %7s", &s, &e, perms) < 3)
            continue;
        if (strncmp(perms, "rw", 2) != 0)
            continue;
        if (strstr(line, "[stack]") || strstr(line, "[vector]") || strstr(line, "[vdso]"))
            continue;
        if (e - s < 0x3000)
            continue;
        if (s < 0x10000000UL)
            continue;
        candidates[nc++] = (s + 0x1000) & ~0xFULL;
    }
    pclose(fp);
    if (!nc) {
        fprintf(stderr, "[-] 无合适的 rw-p 段写 path\n");
        return 0;
    }

    snprintf(mempath, sizeof(mempath), "/proc/%d/mem", (int)pid);
    fd = open(mempath, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        struct iovec local, remote;
        for (i = 0; i < nc; i++) {
            local.iov_base = (void *)path;
            local.iov_len = plen;
            remote.iov_base = (void *)(uintptr_t)candidates[i];
            remote.iov_len = plen;
            if (process_vm_writev(pid, &local, 1, &remote, 1, 0) == (ssize_t)plen) {
                local.iov_base = verify;
                if (process_vm_readv(pid, &local, 1, &remote, 1, 0) == (ssize_t)plen &&
                    memcmp(verify, path, plen) == 0) {
                    printf("[+] remote path @0x%lx via process_vm_writev\n", candidates[i]);
                    return candidates[i];
                }
            }
        }
        fprintf(stderr, "[-] open mem errno=%d, process_vm_* also failed\n", errno);
        return 0;
    }

    for (i = 0; i < nc; i++) {
        if (lseek(fd, (off_t)candidates[i], SEEK_SET) < 0)
            continue;
        n = (int)write(fd, path, plen);
        if (n != (int)plen)
            continue;
        if (lseek(fd, (off_t)candidates[i], SEEK_SET) < 0)
            continue;
        memset(verify, 0, sizeof(verify));
        if (read(fd, verify, plen) != (ssize_t)plen)
            continue;
        if (memcmp(verify, path, plen) != 0)
            continue;
        close(fd);
        printf("[+] remote path @0x%lx ok (%s)\n", candidates[i], path);
        return candidates[i];
    }
    close(fd);
    fprintf(stderr, "[-] 所有候选地址写 path 校验失败\n");
    return 0;
}

static int kpm_load(const char *kpm_path)
{
    char cmd[768];
    if (!g_key || !g_key[0])
        return -1;
    if (access(g_path_kpatch, X_OK) != 0 || access(kpm_path, R_OK) != 0)
        return -1;
    snprintf(cmd, sizeof(cmd), "%s '%s' kpm load %s 2>/dev/null", g_path_kpatch, g_key,
             kpm_path);
    system(cmd);
    usleep(200 * 1000);
    return 0;
}

static int wxshadow_ping(void)
{
    long r = prctl(0x57580005, 0, 0, 0, 0);
    return r >= 0;
}

/* dbc-rw：syscall 41 magic VERSION */
static int dbc_rw_ping(void)
{
    register long x8 __asm__("x8") = 41;
    register long x0 __asm__("x0") = (long)(0x1b1841fd2c1e0000ULL);
    register long x1 __asm__("x1") = 0;
    __asm__ __volatile__("svc 0" : "+r"(x0) : "r"(x8), "r"(x1) : "memory", "cc");
    return x0 > 0;
}

static int ensure_modules(void)
{
    if ((!hide_ping() || !kload_ping() || !wxshadow_ping()) && g_key && g_key[0] &&
        file_ok(g_path_stealth)) {
        printf("[*] loading stealth.kpm (hm+kload+wx+rw) ...\n");
        printf("    %s\n", g_path_stealth);
        kpm_load(g_path_stealth);
        usleep(250 * 1000);
    }

    if (!hide_ping()) {
        printf("[*] loading hide-maps ...\n");
        kpm_load(g_path_hide);
        if (!hide_ping()) {
            fprintf(stderr, "[-] hide-maps 未就绪\n");
            return -1;
        }
    }
    printf("[+] hide-maps ready\n");

    if (!g_force_tinject && !kload_ping()) {
        printf("[*] loading kload ...\n");
        kpm_load(g_path_kload);
        if (kload_ping())
            printf("[+] kload ready\n");
        else
            printf("[!] kload 未加载，将回退 tinjector\n");
    } else if (kload_ping()) {
        printf("[+] kload ready\n");
    }

    if (!wxshadow_ping()) {
        printf("[*] loading wxshadow ...\n");
        kpm_load(g_path_wx);
        if (wxshadow_ping())
            printf("[+] wxshadow ready\n");
        else
            printf("[!] wxshadow 未就绪（khook 将失败）\n");
    } else {
        printf("[+] wxshadow ready\n");
    }

    if (dbc_rw_ping())
        printf("[+] dbc-rw ready (syscall 41)\n");
    else
        printf("[!] dbc-rw 未探测到（可仅注入 hook，外部读写不可用）\n");

    return 0;
}

/* ---------- 进程 ---------- */
static int cmdline_match(pid_t pid, const char *pkg)
{
    char path[64], buf[256];
    int fd, n, i;
    snprintf(path, sizeof(path), "/proc/%d/cmdline", (int)pid);
    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return 0;
    n = (int)read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0)
        return 0;
    buf[n] = '\0';
    if (strcmp(buf, pkg) == 0)
        return 1;
    for (i = 0; i < n - 1; i++)
        if (buf[i] == '\0' && strcmp(buf + i + 1, pkg) == 0)
            return 1;
    return strstr(buf, pkg) != NULL;
}

static int find_mod_base(pid_t pid, const char *substr, unsigned long *base_out,
                         char *path_out, size_t path_len)
{
    char cmd[96], line[512];
    FILE *fp;
    *base_out = 0;
    snprintf(cmd, sizeof(cmd), "cat /proc/%d/maps 2>/dev/null", (int)pid);
    fp = popen(cmd, "r");
    if (!fp)
        return -1;
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
        if (*base_out == 0 || s < *base_out) {
            *base_out = s;
            if (path_out && path_len) {
                strncpy(path_out, path, path_len - 1);
                path_out[path_len - 1] = 0;
            }
        }
    }
    pclose(fp);
    return *base_out ? 0 : -1;
}

static int maps_has_lib(pid_t pid, const char *name)
{
    unsigned long b = 0;
    char p[64];
    if (!name || !name[0])
        return 1;
    return find_mod_base(pid, name, &b, p, sizeof(p)) == 0;
}

static pid_t find_ready_package_pid(const char *pkg)
{
    DIR *d = opendir("/proc");
    struct dirent *e;
    pid_t best = 0;
    if (!d)
        return 0;
    while ((e = readdir(d))) {
        char *end = NULL;
        pid_t pid;
        unsigned long b = 0;
        char p[64] = {0};
        if (e->d_name[0] < '1' || e->d_name[0] > '9')
            continue;
        pid = (pid_t)strtol(e->d_name, &end, 10);
        if (!end || *end || pid <= 1)
            continue;
        if (!cmdline_match(pid, pkg))
            continue;
        if (find_mod_base(pid, "libdl.so", &b, p, sizeof(p)) == 0 ||
            find_mod_base(pid, "linker64", &b, p, sizeof(p)) == 0) {
            best = pid;
            break;
        }
        if (!best)
            best = pid;
    }
    closedir(d);
    return best;
}

static pid_t find_package_pid(const char *pkg)
{
    DIR *d = opendir("/proc");
    struct dirent *e;
    pid_t found = 0;
    if (!d)
        return 0;
    while ((e = readdir(d))) {
        char *end = NULL;
        pid_t pid;
        if (e->d_name[0] < '1' || e->d_name[0] > '9')
            continue;
        pid = (pid_t)strtol(e->d_name, &end, 10);
        if (!end || *end || pid <= 1)
            continue;
        if (cmdline_match(pid, pkg)) {
            found = pid;
            break;
        }
    }
    closedir(d);
    return found;
}

static void start_package(const char *pkg)
{
    char cmd[384];
    snprintf(cmd, sizeof(cmd), "am force-stop %s >/dev/null 2>&1", pkg);
    system(cmd);
    usleep(400 * 1000);
    snprintf(cmd, sizeof(cmd),
             "monkey -p %s -c android.intent.category.LAUNCHER 1 >/dev/null 2>&1", pkg);
    system(cmd);
}

static long local_sym_offset(const char *libpath, const char *sym)
{
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

    fd = open(libpath, O_RDONLY);
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
        for (size_t j = 0; j < nsym; j++) {
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

static unsigned long resolve_remote_dlopen(pid_t pid)
{
    static const char *libs[] = {"libdl.so", "linker64", "/bin/linker64", "linker", NULL};
    static const char *syms[] = {"dlopen", "android_dlopen_ext", "__loader_dlopen", NULL};
    int li, si, attempt;

    for (attempt = 0; attempt < 30; attempt++) {
        for (li = 0; libs[li]; li++) {
            unsigned long base = 0;
            char path[256] = {0};
            if (find_mod_base(pid, libs[li], &base, path, sizeof(path)) < 0)
                continue;
            if (!path[0])
                continue;
            printf("[*] maps hit %s base=0x%lx path=%s\n", libs[li], base, path);
            for (si = 0; syms[si]; si++) {
                long off = local_sym_offset(path, syms[si]);
                if (off > 0) {
                    unsigned long va = base + (unsigned long)off;
                    printf("[+] remote %s!%s = 0x%lx\n", libs[li], syms[si], va);
                    return va;
                }
            }
        }
        usleep(100 * 1000);
    }
    fprintf(stderr, "[-] 无法解析目标进程 dlopen 地址 (pid=%d)\n", (int)pid);
    return 0;
}

static unsigned long resolve_self_dlopen_as_template(pid_t pid)
{
    void *h = dlopen("libdl.so", RTLD_NOW);
    void *sym = h ? dlsym(h, "android_dlopen_ext") : NULL;
    if (!sym && h)
        sym = dlsym(h, "dlopen");
    if (!sym)
        return 0;

    unsigned long self_base = 0, tgt_base = 0;
    char path[256] = {0};
    Dl_info info;
    if (!dladdr(sym, &info) || !info.dli_fbase)
        return 0;
    self_base = (unsigned long)info.dli_fbase;
    if (find_mod_base(pid, "libdl.so", &tgt_base, path, sizeof(path)) < 0)
        return 0;
    return tgt_base + ((unsigned long)sym - self_base);
}

static int get_proc_uid(pid_t pid, uid_t *uid_out)
{
    char path[64], line[128];
    FILE *fp;
    snprintf(path, sizeof(path), "/proc/%d/status", (int)pid);
    fp = fopen(path, "r");
    if (!fp)
        return -1;
    while (fgets(line, sizeof(line), fp)) {
        unsigned u = 0;
        if (sscanf(line, "Uid: %u", &u) == 1) {
            *uid_out = (uid_t)u;
            fclose(fp);
            return 0;
        }
    }
    fclose(fp);
    return -1;
}

static int stage_so_for_app(pid_t pid, const char *pkg, const char *src_so, char *out_path,
                            size_t out_len)
{
    uid_t uid = 0;
    char cmd[1024], dir[256], rnd_name[32];
    unsigned r = 0;
    int fd, st;
    const char *roots[2];
    int ri;

    if (get_proc_uid(pid, &uid) < 0) {
        fprintf(stderr, "[-] 读 Uid 失败 pid=%d\n", (int)pid);
        return -1;
    }

    fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        if (read(fd, &r, sizeof(r)) != (ssize_t)sizeof(r))
            r = (unsigned)time(NULL) ^ (unsigned)pid;
        close(fd);
    } else {
        r = (unsigned)time(NULL) ^ (unsigned)pid ^ (unsigned)getpid();
    }
    snprintf(rnd_name, sizeof(rnd_name), "%08x", r);

    roots[0] = "/data/data";
    roots[1] = "/data/user/0";
    for (ri = 0; ri < 2; ri++) {
        snprintf(dir, sizeof(dir), "%s/%s/code_cache", roots[ri], pkg);
        snprintf(out_path, out_len, "%s/%s", dir, rnd_name);
        snprintf(cmd, sizeof(cmd),
                 "mkdir -p '%s' && cp -f '%s' '%s' && chown %u:%u '%s' && "
                 "chmod 755 '%s' && restorecon -RF '%s' 2>/dev/null; test -f '%s'",
                 dir, src_so, out_path, (unsigned)uid, (unsigned)uid, out_path, out_path,
                 dir, out_path);
        printf("[*] stage so → %s (uid=%u)\n", out_path, (unsigned)uid);
        st = system(cmd);
        if (WIFEXITED(st) && WEXITSTATUS(st) == 0)
            break;
        out_path[0] = 0;
    }
    if (!out_path[0]) {
        fprintf(stderr, "[-] stage so 失败\n");
        return -1;
    }

    hide_add_kw(out_path);
    hide_add_kw(rnd_name);
    {
        char del[340];
        snprintf(del, sizeof(del), "%s (deleted)", out_path);
        hide_add_kw(del);
        snprintf(del, sizeof(del), "%s (deleted)", rnd_name);
        hide_add_kw(del);
    }
    return 0;
}

static void unlink_staged_so(const char *staged)
{
    if (!staged || staged[0] != '/' || !strstr(staged, "/code_cache/"))
        return;
    if (unlink(staged) == 0) {
        printf("[+] unlinked staged so: %s\n", staged);
        return;
    }
    {
        char cmd[400];
        snprintf(cmd, sizeof(cmd), "rm -f '%s'", staged);
        system(cmd);
    }
}

static int proc_alive(pid_t pid)
{
    char p[64];
    snprintf(p, sizeof(p), "/proc/%d", (int)pid);
    return access(p, F_OK) == 0;
}

static int do_kload_inject(pid_t pid, const char *pkg, const char *so)
{
    unsigned long dl;
    int st, i;
    char staged[320];

    if (stage_so_for_app(pid, pkg, so, staged, sizeof(staged)) < 0) {
        printf("[!] stage 失败，仍尝试原路径 %s\n", so);
        strncpy(staged, so, sizeof(staged) - 1);
        staged[sizeof(staged) - 1] = 0;
    } else {
        printf("[+] staged: %s\n", staged);
    }

    dl = resolve_remote_dlopen(pid);
    if (!dl)
        dl = resolve_self_dlopen_as_template(pid);
    if (!dl)
        return -1;

    {
        unsigned long path_u = write_path_remote(pid, staged);
        if (!path_u) {
            fprintf(stderr, "[-] 无法把 path 写入目标进程内存\n");
            return -1;
        }
        if (kload_inject(pid, path_u, dl) != 0) {
            fprintf(stderr, "[-] kload INJECT prctl failed errno=%d\n", errno);
            return -1;
        }
    }
    printf("[*] kload armed — 等目标 syscall 触发 dlopen ...\n");

    for (i = 0; i < 120; i++) {
        st = kload_status();
        if (st == 2) {
            printf("[+] kload hijack done (status=2)\n");
            usleep(800 * 1000);
            if (!proc_alive(pid)) {
                fprintf(stderr, "[-] hijack 后进程已死\n");
                return -1;
            }
            usleep(400 * 1000);
            {
                const char *base = strrchr(staged, '/');
                base = base ? base + 1 : staged;
                int nr = hide_register_so_ranges(pid, base);
                if (nr == 0) {
                    usleep(500 * 1000);
                    nr = hide_register_so_ranges(pid, base);
                }
                printf("[*] hide so ranges registered: %d\n", nr);
            }
            if (strstr(staged, "/code_cache/"))
                unlink_staged_so(staged);
            {
                const char *base = strrchr(staged, '/');
                base = base ? base + 1 : staged;
                usleep(150 * 1000);
                hide_register_so_ranges(pid, base);
            }
            return 0;
        }
        if (st < 0) {
            fprintf(stderr, "[-] kload error status=%d\n", st);
            return -1;
        }
        if (!proc_alive(pid)) {
            fprintf(stderr, "[-] 等待 hijack 时进程已死\n");
            return -1;
        }
        usleep(50 * 1000);
    }
    printf("[!] kload timeout status=%d\n", kload_status());
    return -1;
}

static int do_tinject(pid_t pid, const char *pkg, const char *so)
{
    char cmd[768];
    int st;
    if (access(g_path_tinject, X_OK) != 0) {
        fprintf(stderr, "[-] tinjector 不存在: %s\n", g_path_tinject);
        return -1;
    }
    snprintf(cmd, sizeof(cmd), "cd /data/local/tmp && %s --hide --hide1 -p %s %s 2>&1",
             g_path_tinject, pkg, so);
    printf("[*] tinject: %s\n", cmd);
    st = system(cmd);
    if (WIFEXITED(st) && WEXITSTATUS(st) == 0)
        return 0;
    snprintf(cmd, sizeof(cmd), "cd /data/local/tmp && %s --hide --hide1 -p %d %s 2>&1",
             g_path_tinject, (int)pid, so);
    st = system(cmd);
    return (WIFEXITED(st) && WEXITSTATUS(st) == 0) ? 0 : -1;
}

/* 解析 so 为绝对路径 */
static int resolve_so_path(const char *in)
{
    if (!in || !in[0]) {
        if (file_ok(g_path_so_default)) {
            snprintf(g_so_buf, sizeof(g_so_buf), "%s", g_path_so_default);
            g_so = g_so_buf;
            return 0;
        }
        fprintf(stderr, "[-] 未指定 -s 且默认 so 不存在\n");
        return -1;
    }
    if (in[0] == '/') {
        snprintf(g_so_buf, sizeof(g_so_buf), "%s", in);
        g_so = g_so_buf;
        return file_ok(g_so) ? 0 : -1;
    }
    /* 相对：先 self_dir 再 cwd 再 tmp */
    path_join(g_so_buf, sizeof(g_so_buf), g_self_dir, in);
    if (file_ok(g_so_buf)) {
        g_so = g_so_buf;
        return 0;
    }
    if (realpath(in, g_so_buf)) {
        g_so = g_so_buf;
        return file_ok(g_so) ? 0 : -1;
    }
    path_join(g_so_buf, sizeof(g_so_buf), "/data/local/tmp", in);
    g_so = g_so_buf;
    return file_ok(g_so) ? 0 : -1;
}

int main(int argc, char **argv)
{
    char kw[64];
    const char *b;
    pid_t pid = 0;
    time_t t0;
    const char *so_arg = NULL;

    resolve_self_dir(argv[0]);
    init_default_paths();

    for (int i = 1; i < argc; i++) {
        if ((!strcmp(argv[i], "-p") || !strcmp(argv[i], "--package")) && i + 1 < argc)
            g_pkg = argv[++i];
        else if ((!strcmp(argv[i], "-s") || !strcmp(argv[i], "--so")) && i + 1 < argc)
            so_arg = argv[++i];
        else if (!strcmp(argv[i], "--start"))
            g_do_start = 1;
        else if (!strcmp(argv[i], "--key") && i + 1 < argc)
            g_key = argv[++i];
        else if (!strcmp(argv[i], "--kpm") && i + 1 < argc)
            snprintf(g_path_stealth, sizeof(g_path_stealth), "%s", argv[++i]);
        else if (!strcmp(argv[i], "--kpatch") && i + 1 < argc)
            snprintf(g_path_kpatch, sizeof(g_path_kpatch), "%s", argv[++i]);
        else if (!strcmp(argv[i], "--wait-lib") && i + 1 < argc)
            g_wait_lib = argv[++i];
        else if (!strcmp(argv[i], "--delay") && i + 1 < argc)
            g_delay_ms = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--tinject"))
            g_force_tinject = 1;
        else if (!strcmp(argv[i], "--no-tinject"))
            g_no_tinject = 1;
        else if (!strcmp(argv[i], "--timeout") && i + 1 < argc)
            g_timeout_sec = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            usage(argv[0]);
            return 0;
        } else {
            usage(argv[0]);
            return 1;
        }
    }

    if (resolve_so_path(so_arg) != 0) {
        fprintf(stderr, "[-] so 不可用: %s\n", so_arg ? so_arg : g_path_so_default);
        return 1;
    }

    printf("=== stealth_inject (dbc_lostgo_hook) ===\n");
    printf("  package : %s\n", g_pkg);
    printf("  so      : %s\n", g_so);
    printf("  wait-lib: %s\n", g_wait_lib && g_wait_lib[0] ? g_wait_lib : "(none)");
    printf("  delay   : %d ms\n", g_delay_ms);
    printf("  kpatch  : %s\n", g_path_kpatch);
    printf("  kpm     : %s\n", g_path_stealth);

    if (ensure_modules() < 0)
        return 2;

    if (prctl(PR_HIDEMAPS_CLR_RANGE, 0, 0, 0, 0) == 0)
        printf("[+] hide-maps ranges cleared\n");
    else
        printf("[!] CLR_RANGE failed (可继续)\n");

    if (hide_register(g_pkg, g_so, g_do_start ? 1 : 0) != 0)
        printf("[!] hide REGISTER 失败（可继续）\n");
    else
        printf("[+] hide-maps job registered\n");

    b = strrchr(g_so, '/');
    b = b ? b + 1 : g_so;
    strncpy(kw, b, sizeof(kw) - 1);
    kw[sizeof(kw) - 1] = '\0';
    if (kw[0])
        hide_add_kw(kw);
    if (strcmp(kw, "libdbc.so") != 0)
        hide_add_kw("libdbc.so");

    if (g_do_start) {
        printf("[*] restart app ...\n");
        start_package(g_pkg);
    } else {
        printf("[*] wait process ...\n");
    }

    t0 = time(NULL);
    while (1) {
        unsigned long bb = 0;
        char pp[256] = {0};
        pid = find_ready_package_pid(g_pkg);
        if (pid > 0 &&
            (find_mod_base(pid, "libdl.so", &bb, pp, sizeof(pp)) == 0 ||
             find_mod_base(pid, "linker64", &bb, pp, sizeof(pp)) == 0)) {
            /* 额外等待游戏 lib（默认 libUE4.so） */
            if (g_wait_lib && g_wait_lib[0] && !maps_has_lib(pid, g_wait_lib)) {
                static time_t last_print;
                if (time(NULL) != last_print) {
                    printf("[*] pid=%d 等待 %s ...\n", (int)pid, g_wait_lib);
                    last_print = time(NULL);
                }
            } else {
                printf("[+] ready pid=%d libdl=%s\n", (int)pid, pp);
                if (g_wait_lib && g_wait_lib[0])
                    printf("[+] wait-lib ok: %s\n", g_wait_lib);
                break;
            }
        }
        if (time(NULL) - t0 >= g_timeout_sec) {
            fprintf(stderr, "[-] timeout waiting ready process\n");
            return 4;
        }
        usleep((useconds_t)g_poll_us);
    }

    if (g_delay_ms > 0) {
        printf("[*] delay %d ms before inject\n", g_delay_ms);
        usleep((useconds_t)g_delay_ms * 1000);
        if (!proc_alive(pid)) {
            fprintf(stderr, "[-] delay 后进程已死\n");
            return 5;
        }
    }

    if (!g_force_tinject && kload_ping()) {
        if (do_kload_inject(pid, g_pkg, g_so) == 0) {
            printf("[+] kernel inject OK (no ptrace)\n");
            printf("    logcat -s DbcHK ; example_rw -p %s -m libUE4.so -o 0\n", g_pkg);
            return 0;
        }
        printf("[!] kload 失败%s\n", g_no_tinject ? "" : "，回退 tinjector");
        if (g_no_tinject)
            return 5;
        if (!proc_alive(pid) || !find_package_pid(g_pkg)) {
            printf("[*] 进程已死，重拉再 tinject\n");
            start_package(g_pkg);
            t0 = time(NULL);
            pid = 0;
            while (time(NULL) - t0 < 45) {
                pid = find_ready_package_pid(g_pkg);
                if (pid > 0 && maps_has_lib(pid, "libdl.so") &&
                    (!g_wait_lib || !g_wait_lib[0] || maps_has_lib(pid, g_wait_lib)))
                    break;
                usleep(100 * 1000);
            }
        } else {
            pid = find_package_pid(g_pkg);
        }
        if (pid <= 0) {
            fprintf(stderr, "[-] 回退时无进程\n");
            return 5;
        }
    }

    if (do_tinject(pid, g_pkg, g_so) == 0) {
        printf("[+] tinjector OK (fallback)\n");
        return 0;
    }
    fprintf(stderr, "[-] all inject methods failed\n");
    return 5;
}
