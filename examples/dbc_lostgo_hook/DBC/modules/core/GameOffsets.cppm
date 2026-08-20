module;

#include <cstdint>

export module GameOffsets;

export namespace Offsets {

// --- 全局静态地址（相对 libUE4.so）---
inline constexpr int64_t World            = 0x11CE4FF0;
inline constexpr int64_t GName            = 0x11B4EC40 + 0x40;
inline constexpr int64_t StaticFindObject = 0xBD53BC8;
inline constexpr int64_t GEngine          = 0x11CE0498;
inline constexpr int64_t GNamePool        = 0x11B4EC40;

} // namespace Offsets
