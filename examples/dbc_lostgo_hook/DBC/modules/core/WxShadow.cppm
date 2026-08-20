module;

#include <android/log.h>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <unistd.h>
#include <vector>
#include <algorithm>

export module WxShadow;

/**
 * 渲染函数塞日志 — 内核 shadow + 近邻 stub
 *
 * 【关键】游戏代码页是 W^X shadow：
 *   - 执行看 shadow，读看 original
 *   - 入口禁止 LDR/literal/BR-abs（会读 code 页 → 读到原指令当立即数 → 跳飞卡死）
 *   - 入口只能用纯 B imm（只取指、不读数据）
 *
 * stub 在自建匿名页（读写执行同一视图），里面可以 LDR/BLR。
 */

namespace {

constexpr const char* kTag = "DbcHK";
#define WX_LOGI(...) __android_log_print(ANDROID_LOG_INFO, kTag, __VA_ARGS__)
#define WX_LOGE(...) __android_log_print(ANDROID_LOG_ERROR, kTag, __VA_ARGS__)

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

constexpr int PR_WXSHADOW_PATCH        = 0x57580006;
constexpr int PR_WXSHADOW_RELEASE      = 0x57580008;
constexpr int PR_WXSHADOW_GET_TLB_MODE = 0x57580005;
constexpr size_t kPageSize = 4096;
constexpr uintptr_t kBReach = (1u << 27);

struct HookRec {
    uintptr_t target{};
    void*     page{};
    size_t    stolen{};
};

std::mutex g_mu;
std::vector<HookRec> g_hooks;
int g_avail = -1;
std::atomic<uint64_t> g_frame{0};

/* 用户业务：在目标函数里、跑原逻辑之前调用（C 函数指针） */
using EnterFn = void (*)();
std::atomic<EnterFn> g_enter_fn{nullptr};

/* 页对齐，仅用于把 4 字节 B 交给内核 */
alignas(kPageSize) uint8_t g_patch_buf[kPageSize];

/**
 * 业务回调（render 热路径）。
 * 不在此改 x29/x30：假帧伪装会导致 UE4 界面卡死。
 */
extern "C" void OnFrameEnter(void) {
    const uint64_t n = g_frame.fetch_add(1, std::memory_order_relaxed) + 1;

    if (n <= 3 || (n % 600) == 0) {
        __android_log_print(ANDROID_LOG_INFO, kTag,
                            ">>> HOOK 已进函数 frame=%llu (no stack-spoof)",
                            static_cast<unsigned long long>(n));
    }

    if (EnterFn fn = g_enter_fn.load(std::memory_order_acquire)) {
        fn();
    }
}

void UninstallAllHooks();

void EnsureAutoCleanupOnce() {
    static std::once_flag once;
    std::call_once(once, [] { std::atexit([] { UninstallAllHooks(); }); });
}

void UninstallAllHooks() {
    std::vector<HookRec> copy;
    {
        std::lock_guard lock(g_mu);
        if (g_hooks.empty()) return;
        copy.swap(g_hooks);
    }
    for (auto& h : copy) {
        ::prctl(PR_WXSHADOW_RELEASE, 0L, static_cast<unsigned long>(h.target), 0L, 0L);
        if (h.page) ::munmap(h.page, kPageSize);
    }
    WX_LOGI("WxShadow: auto-uninstall %zu hooks", copy.size());
}

inline void FlushI(void* p, size_t n) {
    __builtin___clear_cache(static_cast<char*>(p), static_cast<char*>(p) + n);
}

inline bool InBRange(uintptr_t from, uintptr_t to) {
    const intptr_t d = static_cast<intptr_t>(to - from);
    return d >= -static_cast<intptr_t>(kBReach) &&
           d < static_cast<intptr_t>(kBReach) && (d & 3) == 0;
}

inline uint32_t EncB(uintptr_t from, uintptr_t to) {
    const intptr_t imm = static_cast<intptr_t>(to - from) >> 2;
    return 0x14000000u | (static_cast<uint32_t>(imm) & 0x03FFFFFFu);
}

inline uint32_t EncStp(int rt, int rt2, int imm) {
    return 0xA9000000u | (((imm / 8) & 0x7F) << 15) | (rt2 << 10) | (31 << 5) | rt;
}
inline uint32_t EncLdp(int rt, int rt2, int imm) {
    return 0xA9400000u | (((imm / 8) & 0x7F) << 15) | (rt2 << 10) | (31 << 5) | rt;
}
inline uint32_t EncStpQ(int rt, int rt2, int imm) {
    return 0xAD000000u | (((imm / 16) & 0x7F) << 15) | (rt2 << 10) | (31 << 5) | rt;
}
inline uint32_t EncLdpQ(int rt, int rt2, int imm) {
    return 0xAD400000u | (((imm / 16) & 0x7F) << 15) | (rt2 << 10) | (31 << 5) | rt;
}
inline uint32_t EncStrX(int rt, int imm) {
    return 0xF9000000u | (((imm / 8) & 0xFFF) << 10) | (31 << 5) | rt;
}
inline uint32_t EncLdrX(int rt, int imm) {
    return 0xF9400000u | (((imm / 8) & 0xFFF) << 10) | (31 << 5) | rt;
}
inline uint32_t EncSubSp(int imm) {
    return 0xD1000000u | ((imm & 0xFFF) << 10) | (31 << 5) | 31;
}
inline uint32_t EncAddSp(int imm) {
    return 0x91000000u | ((imm & 0xFFF) << 10) | (31 << 5) | 31;
}
inline uint32_t EncLdrLit(int rt, int byte_off) {
    return 0x58000000u | (((byte_off / 4) & 0x7FFFF) << 5) | rt;
}
inline uint32_t EncBlr(int rn) {
    return 0xD63F0000u | (rn << 5);
}
/* BTI jc — 间接跳转落地（stub 被 B 进入也无妨） */
constexpr uint32_t kBtiJc = 0xD503245Fu;

bool IsPcRel(uint32_t insn) {
    if ((insn & 0x7C000000u) == 0x14000000u) return true;
    if ((insn & 0xFF000010u) == 0x54000000u) return true;
    if ((insn & 0x7E000000u) == 0x34000000u) return true;
    if ((insn & 0x7E000000u) == 0x36000000u) return true;
    if ((insn & 0x9F000000u) == 0x10000000u) return true;
    if ((insn & 0x9F000000u) == 0x90000000u) return true;
    if ((insn & 0x3B000000u) == 0x18000000u) return true;
    return false;
}

bool PrologueOk(uintptr_t target, size_t stolen) {
    if ((target & (kPageSize - 1)) + stolen > kPageSize) return false;
    const auto* src = reinterpret_cast<const uint32_t*>(target);
    for (size_t i = 0; i < stolen / 4; ++i)
        if (IsPcRel(src[i])) return false;
    return true;
}

struct Vma {
    uintptr_t start{};
    uintptr_t end{};
    bool      protnone{}; /* ---p 保留区：可 MAP_FIXED 拆页占用 */
};

std::vector<Vma> ReadMaps() {
    std::vector<Vma> out;
    FILE* fp = std::fopen("/proc/self/maps", "r");
    if (!fp) return out;
    char line[768];
    while (std::fgets(line, sizeof(line), fp)) {
        unsigned long s = 0, e = 0;
        char perms[8] = {};
        if (std::sscanf(line, "%lx-%lx %7s", &s, &e, perms) >= 3)
            out.push_back({s, e, perms[0] == '-' && perms[1] == '-' && perms[2] == '-'});
    }
    std::fclose(fp);
    return out;
}

/* allow_fixed：覆盖 PROT_NONE 保留页（游戏附近几乎无真空洞，必须靠这个） */
void* TryMapAt(uintptr_t addr, bool allow_fixed) {
    addr &= ~(kPageSize - 1);
    if (addr < 0x10000) return nullptr;
    void* p = ::mmap(reinterpret_cast<void*>(addr), kPageSize,
                     PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE,
                     -1, 0);
    if (p != MAP_FAILED) return p;
    if (!allow_fixed) return nullptr;
    p = ::mmap(reinterpret_cast<void*>(addr), kPageSize,
               PROT_READ | PROT_WRITE,
               MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED,
               -1, 0);
    return p == MAP_FAILED ? nullptr : p;
}

void* TryNearPage(uintptr_t target, uintptr_t addr, bool allow_fixed) {
    if (!InBRange(target, addr)) return nullptr;
    void* p = TryMapAt(addr, allow_fixed);
    if (!p) return nullptr;
    /* 必须能 B 进、B 回（回跳约在 page+0x100） */
    if (!InBRange(target, reinterpret_cast<uintptr_t>(p)) ||
        !InBRange(reinterpret_cast<uintptr_t>(p) + 0x100, target + 4)) {
        ::munmap(p, kPageSize);
        return nullptr;
    }
    WX_LOGI("WxShadow: hole %p dist=%ldKB fixed=%d",
            p, (long)((intptr_t)p - (intptr_t)target) / 1024, allow_fixed ? 1 : 0);
    return p;
}

void* MmapNear(uintptr_t target) {
    WX_LOGI("WxShadow: MmapNear target=0x%lx", (unsigned long)target);
    const auto maps = ReadMaps();

    const uintptr_t win_lo = (target > kBReach) ? (target - kBReach + kPageSize) : kPageSize;
    const uintptr_t win_hi = target + kBReach - kPageSize;

    /* cand: 低位 bit0=1 表示可 MAP_FIXED（来自 PROT_NONE） */
    std::vector<uintptr_t> cands;
    if (maps.size() >= 2) {
        for (size_t i = 0; i + 1 < maps.size(); ++i) {
            uintptr_t gap_s = (maps[i].end + kPageSize - 1) & ~(kPageSize - 1);
            uintptr_t gap_e = maps[i + 1].start & ~(kPageSize - 1);
            if (gap_e < gap_s + kPageSize) continue;
            uintptr_t a0 = gap_s < win_lo ? win_lo : gap_s;
            uintptr_t a1 = gap_e > win_hi ? win_hi : gap_e;
            a0 = (a0 + kPageSize - 1) & ~(kPageSize - 1);
            if (a1 < a0 + kPageSize) continue;
            int added = 0;
            for (uintptr_t a = a0; a + kPageSize <= a1 && added < 48; a += kPageSize) {
                if (!InBRange(target, a)) continue;
                cands.push_back(a);
                ++added;
            }
        }
    }
    /* PROT_NONE 内部页：地址空间被占但可 FIXED 拆页 */
    for (const auto& m : maps) {
        if (!m.protnone || m.end - m.start < kPageSize * 2) continue;
        uintptr_t a0 = m.start + kPageSize;
        uintptr_t a1 = m.end - kPageSize;
        if (a0 < win_lo) a0 = win_lo;
        if (a1 > win_hi) a1 = win_hi;
        a0 = (a0 + kPageSize - 1) & ~(kPageSize - 1);
        int added = 0;
        for (uintptr_t a = a0; a + kPageSize <= a1 && added < 40; a += kPageSize) {
            if (!InBRange(target, a)) continue;
            cands.push_back(a | 1ull);
            ++added;
        }
    }

    std::sort(cands.begin(), cands.end(), [target](uintptr_t a, uintptr_t b) {
        auto aa = a & ~1ull, bb = b & ~1ull;
        auto da = aa > target ? aa - target : target - aa;
        auto db = bb > target ? bb - target : target - bb;
        return da < db;
    });
    cands.erase(std::unique(cands.begin(), cands.end()), cands.end());

    WX_LOGI("WxShadow: %zu candidates (gap+protnone)", cands.size());
    for (size_t i = 0; i < cands.size() && i < 120; ++i) {
        const bool fix = (cands[i] & 1ull) != 0;
        const uintptr_t a = cands[i] & ~1ull;
        if (void* p = TryNearPage(target, a, fix))
            return p;
    }

    /* 回退：两侧扫页，NOREPLACE 失败再 FIXED */
    WX_LOGI("WxShadow: brute-force near pages within B range");
    constexpr int kMaxProbe = 8192;
    for (int n = 1; n <= kMaxProbe; ++n) {
        const uintptr_t off = static_cast<uintptr_t>(n) * kPageSize;
        if (target > off + kPageSize) {
            if (void* p = TryNearPage(target, (target - off) & ~(kPageSize - 1), false))
                return p;
            if (void* p = TryNearPage(target, (target - off) & ~(kPageSize - 1), true))
                return p;
        }
        if (void* p = TryNearPage(target, (target + off) & ~(kPageSize - 1), false))
            return p;
        if (void* p = TryNearPage(target, (target + off) & ~(kPageSize - 1), true))
            return p;
    }

    WX_LOGE("WxShadow: 无可用近邻空洞（需要纯 B 入口，禁止 abs）");
    return nullptr;
}

/*
 * stub:
 *   BTI
 *   保存寄存器
 *   blr OnFrameEnter
 *   恢复
 *   [stolen 原指令 × N]
 *   B target+stolen   // 纯 B，回到 shadow 后续
 */
bool BuildStub(uintptr_t target, size_t stolen, void** page_out, uintptr_t* entry_out) {
    void* page = MmapNear(target);
    if (!page) return false;

    auto* w = static_cast<uint32_t*>(page);
    int i = 0;
    constexpr int kFrame = 0x120;

    w[i++] = kBtiJc;
    w[i++] = EncSubSp(kFrame);
    for (int r = 0; r <= 16; r += 2)
        w[i++] = EncStp(r, r + 1, r * 8);
    w[i++] = EncStrX(18, 0x90);
    w[i++] = EncStrX(30, 0x98);
    for (int q = 0; q <= 6; q += 2)
        w[i++] = EncStpQ(q, q + 1, 0xA0 + q * 16);

    const int ldr_idx = i;
    w[i++] = 0; /* LDR X16, =OnFrameEnter */
    w[i++] = EncBlr(16);

    for (int q = 6; q >= 0; q -= 2)
        w[i++] = EncLdpQ(q, q + 1, 0xA0 + q * 16);
    w[i++] = EncLdrX(30, 0x98);
    w[i++] = EncLdrX(18, 0x90);
    for (int r = 16; r >= 0; r -= 2)
        w[i++] = EncLdp(r, r + 1, r * 8);
    w[i++] = EncAddSp(kFrame);

    const auto* src = reinterpret_cast<const uint32_t*>(target);
    for (size_t s = 0; s < stolen / 4; ++s) {
        WX_LOGI("WxShadow: stolen[%zu]=0x%08X", s, src[s]);
        w[i++] = src[s];
    }

    const int b_idx = i;
    w[i++] = 0;
    if (i & 1) w[i++] = 0xD503201Fu;
    const int lit_idx = i;
    *reinterpret_cast<uint64_t*>(&w[i]) = reinterpret_cast<uint64_t>(&OnFrameEnter);
    i += 2;

    /* LDR 在 stub 页，读写同视图，安全 */
    w[ldr_idx] = EncLdrLit(16, (lit_idx - ldr_idx) * 4);
    {
        const uintptr_t from = reinterpret_cast<uintptr_t>(&w[b_idx]);
        const uintptr_t to = target + stolen;
        if (!InBRange(from, to)) {
            WX_LOGE("WxShadow: B-back OOR");
            ::munmap(page, kPageSize);
            return false;
        }
        w[b_idx] = EncB(from, to);
    }

    if (::mprotect(page, kPageSize, PROT_READ | PROT_EXEC) != 0) {
        ::munmap(page, kPageSize);
        return false;
    }
    FlushI(page, static_cast<size_t>(i) * 4);

    *page_out = page;
    *entry_out = reinterpret_cast<uintptr_t>(page);
    WX_LOGI("WxShadow: stub OK entry=0x%lx", (unsigned long)*entry_out);
    return true;
}

} // namespace

export namespace WxShadow {

/** 设置「进函数后」回调（写 SDK 的地方）；nullptr 清除 */
void SetEnterHandler(void (*fn)()) {
    g_enter_fn.store(fn, std::memory_order_release);
    WX_LOGI("WxShadow: EnterHandler %s", fn ? "set" : "cleared");
}

bool IsAvailable() {
    if (g_avail >= 0) return g_avail == 1;
    errno = 0;
    const long ret = ::prctl(PR_WXSHADOW_GET_TLB_MODE, 0L, 0L, 0L, 0L);
    g_avail = (ret >= 0) ? 1 : 0;
    if (!g_avail)
        WX_LOGE("WxShadow: prctl 不可用 ret=%ld errno=%d", ret, errno);
    else
        WX_LOGI("WxShadow: kpm ready tlb_mode=%ld", ret);
    return g_avail == 1;
}

int InstallFrameLog(void* address) {
    WX_LOGI("WxShadow: InstallFrameLog begin %p", address);
    if (!address) return -1;
    const uintptr_t target = reinterpret_cast<uintptr_t>(address);
    if (target < 0x1000 || !IsAvailable()) return -2;

    std::lock_guard lock(g_mu);
    for (const auto& h : g_hooks)
        if (h.target == target) return 0;

    /*
     * 只偷 1 条指令 + 入口 4 字节纯 B。
     * 绝不能在游戏 shadow 上写 LDR-literal（W^X 读回原页会跳飞）。
     */
    constexpr size_t stolen = 4;
    if (!PrologueOk(target, stolen)) {
        WX_LOGE("WxShadow: 首指令为 PC 相对，放弃");
        return -5;
    }

    void* page = nullptr;
    uintptr_t entry = 0;
    if (!BuildStub(target, stolen, &page, &entry))
        return -6;

    if (!InBRange(target, entry)) {
        WX_LOGE("WxShadow: stub 超出 B 范围，拒绝 abs 回退（W^X 不安全）");
        ::munmap(page, kPageSize);
        return -8;
    }

    std::memset(g_patch_buf, 0, 16);
    const uint32_t b = EncB(target, entry);
    std::memcpy(g_patch_buf, &b, 4);

    WX_LOGI("WxShadow: PATCH B-4 → stub 0x%lx (W^X safe)", (unsigned long)entry);
    ::usleep(30 * 1000);

    const long pret = ::prctl(PR_WXSHADOW_PATCH, 0L,
                              static_cast<unsigned long>(target),
                              reinterpret_cast<unsigned long>(g_patch_buf),
                              4UL);
    WX_LOGI("WxShadow: prctl returned %ld errno=%d", pret, errno);
    if (pret < 0) {
        ::munmap(page, kPageSize);
        return -7;
    }

    EnsureAutoCleanupOnce();
    g_hooks.push_back(HookRec{target, page, stolen});
    WX_LOGI("WxShadow: FrameLog OK 0x%lx — 等 >>> HOOK 已进函数（每帧）",
            (unsigned long)target);
    return 0;
}

int UninstallFrameLog(void* address) {
    if (!address) return -1;
    const uintptr_t target = reinterpret_cast<uintptr_t>(address);
    std::lock_guard lock(g_mu);
    for (auto it = g_hooks.begin(); it != g_hooks.end(); ++it) {
        if (it->target != target) continue;
        ::prctl(PR_WXSHADOW_RELEASE, 0L, static_cast<unsigned long>(target), 0L, 0L);
        if (it->page) ::munmap(it->page, kPageSize);
        g_hooks.erase(it);
        return 0;
    }
    return -1;
}

int UninstallAll() {
    UninstallAllHooks();
    return 0;
}

int KernelHook(void* address, void*, void** out_origin) {
    if (out_origin) *out_origin = nullptr;
    return InstallFrameLog(address);
}

} // namespace WxShadow
