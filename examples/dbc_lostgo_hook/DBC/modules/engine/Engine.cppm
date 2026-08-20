module;

#include <android/log.h>
#include <chrono>
#include <cstdint>
#include <thread>

export module Engine;

import Memory;
import SoList;
import Logger;
import GlobalConfig;
import Hook;

using Core::Ue4Base;

constexpr uintptr_t kRenderOffset = 0xD7493C4;//渲染

void OnRenderEnter() {
    static int once = 0;
       Logger::Debug("OnRenderEnter: SDK/业务逻辑在此执行 (#%d)", once);
}

export void InitEngine() {
    InitMemoryGuard();
    khook::ready();

    std::this_thread::sleep_for(std::chrono::seconds(1));
    if (khook::attach("libUE4.so", kRenderOffset)) {
        Ue4Base = GetModuleBase("libUE4.so");
        Logger::Info("khook: 已塞进 libUE4+0x{:X}  等 log: >>> HOOK 已进函数",kRenderOffset);
    } else {
        Logger::Info("khook: attach 失败");
    }
    khook::on_enter(OnRenderEnter);
    Core::InitOffsets();
    Linker::SetHide_Soinfo("libdbc.so");
}
