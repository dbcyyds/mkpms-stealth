module;

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>

export module Hook;

import Memory;
import WxShadow;

export namespace khook {

/** 进目标函数后、跑原指令前：挂业务（C 函数指针） */
inline void on_enter(void (*fn)()) {
    WxShadow::SetEnterHandler(fn);
}

inline bool ready() {
    return WxShadow::IsAvailable();
}

/** 绝对地址安装 */
inline bool attach(void* addr) {
    if (!addr || !WxShadow::IsAvailable()) return false;
    return WxShadow::InstallFrameLog(addr) == 0;
}

inline bool attach(uintptr_t addr) {
    return attach(reinterpret_cast<void*>(addr));
}

/**
 * 模块+偏移安装（推荐）
 * 等价于：等 so → 入口塞 B → 每帧进 on_enter / 默认 log
 */
inline bool attach(std::string_view module, uintptr_t offset, int timeout_ms = 60000) {
    if (module.empty()) return false;
    const std::string name{module};
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    int64_t base = 0;
    for (;;) {
        base = GetModuleBase(name);
        if (base) break;
        if (timeout_ms <= 0 || std::chrono::steady_clock::now() >= deadline)
            return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return attach(static_cast<uintptr_t>(base) + offset);
}

/** 兼容旧名 */
inline bool log(std::string_view module, uintptr_t offset, int timeout_ms = 60000) {
    return attach(module, offset, timeout_ms);
}
inline bool log(void* addr) { return attach(addr); }
inline bool log(uintptr_t addr) { return attach(addr); }

inline bool remove(void* addr) {
    return addr && WxShadow::UninstallFrameLog(addr) == 0;
}
inline bool remove(uintptr_t addr) {
    return remove(reinterpret_cast<void*>(addr));
}
inline bool remove(std::string_view module, uintptr_t offset) {
    const auto base = GetModuleBase(std::string{module});
    return base && remove(static_cast<uintptr_t>(base) + offset);
}

inline void cleanup() {
    WxShadow::UninstallAll();
}

} // namespace khook

export namespace shadow {
    using HookPointerHandler = std::function<void(void*)>;
    using ProcessEventFn = void(*)(const void*, void*, void*);

    inline ProcessEventFn g_origProcessEvent = nullptr;

    inline void CallOriginalProcessEvent(const void* obj, void* func, void* parms) {
        if (g_origProcessEvent)
            g_origProcessEvent(obj, func, parms);
    }

    template<typename Func, typename... Args>
    inline auto CallGameFunction(Func func, Args&&... args) {
        if (!func) {
            using Ret = decltype(func(std::forward<Args>(args)...));
            if constexpr (std::is_void_v<Ret>) return;
            else return Ret{};
        }
        return func(std::forward<Args>(args)...);
    }

    template<typename FuncType>
    inline FuncType GetVirtualFunction(const void* instance, int index) {
        auto** vtable = *reinterpret_cast<void***>(const_cast<void*>(instance));
        return reinterpret_cast<FuncType>(vtable[index]);
    }
}
