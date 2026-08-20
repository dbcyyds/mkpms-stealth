module;

#include <cstdint>

export module GlobalConfig;

import GameOffsets;
import Memory;

export namespace Core {
    inline int64_t Ue4Base = 0;
    inline int64_t StaticFindObjectAddr = 0;
    inline int64_t GNameAddress = 0;
    inline int64_t GEngineAddress = 0;

    inline void InitOffsets() {
        StaticFindObjectAddr = Ue4Base + Offsets::StaticFindObject;
        GNameAddress = Ue4Base + Offsets::GNamePool;
        GEngineAddress = Ue4Base + Offsets::GEngine;
    }
}
