module;

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>

export module UeObject;

import Hook;
import GlobalConfig;
import Memory;

export namespace UC {
using namespace shadow;
using namespace Core;

class FName final {
public:
    static inline std::unordered_map<int32_t, std::string> Cache{};
    int32_t ComparisonIndex{};
    int32_t Number{};

    FName() = default;
    explicit FName(int32_t i) : ComparisonIndex(i) {}

    std::string ToString() const {
        if (Cache.contains(ComparisonIndex)) return Cache[ComparisonIndex];

        std::string name;
        const uint32_t block = ComparisonIndex >> 16;
        const uint16_t offset = ComparisonIndex & 65535;
        const int64_t namePool = GNameAddress + 0x30;
        const int64_t chunk = Memory::Read<int64_t>(namePool + 0x10 + block * 0x8);
        const int64_t entry = chunk + 0x2 * offset;
        const uint16_t header = Memory::Read<uint16_t>(entry);
        const int strLen = header >> 6;
        if (strLen > 0 && strLen < 250 && !(header & 1))
            name = VmReadString(reinterpret_cast<void*>(entry + 0x2), strLen);
        if (name.empty()) name = "None";

        auto pos = name.rfind('/');
        if (pos != std::string::npos) name = name.substr(pos + 1);
        Cache.try_emplace(ComparisonIndex, name);
        return name;
    }
};

class UClass;
class UFunction;

class UObject {
public:
    using WCHAR = const char16_t;
    using StaticFindObjectType = UObject*(*)(UObject*, int64_t, WCHAR*, bool);

    inline static StaticFindObjectType StaticFindObject = nullptr;
    inline static int64_t* StaticFindObjectAddress = nullptr;

    void** VTable{};
    int32_t Flags{};
    int32_t Index{};
    UClass* Class{};
    FName Name{};
    UObject* Outer{};

    static void Init(const HookPointerHandler& handler) {
        StaticFindObject = reinterpret_cast<StaticFindObjectType>(StaticFindObjectAddr);
        StaticFindObjectAddress = reinterpret_cast<int64_t*>(StaticFindObjectAddr);
        handler(reinterpret_cast<void*>(StaticFindObject));
        handler(StaticFindObjectAddress);
    }

    std::string GetName() const;

    void ProcessEvent(UObject* function, void* parms) const {
        using Fn = void(*)(const UObject*, UObject*, void*);
        CallGameFunction(GetVirtualFunction<Fn>(this, 68), this, function, parms);
    }

    template<typename T>
    static T* FindObject(const std::u16string& name) {
        const auto addr = reinterpret_cast<int64_t>(StaticFindObjectAddress);
        if (*reinterpret_cast<int*>(addr) == 0x58000050) {
            const auto finalAddr = *reinterpret_cast<int64_t*>(addr + 0x8) + 0x150;
            return reinterpret_cast<T*>(
                reinterpret_cast<StaticFindObjectType>(finalAddr)(nullptr, -1, name.data(), false));
        }
        return reinterpret_cast<T*>(StaticFindObject(nullptr, -1, name.data(), false));
    }
};

class UField : public UObject {
public:
    UField* Next{};
};

class UStruct : public UField {
public:
    uint8_t pad0[0x10]{};
    UClass* Super{};
    UField* Children{};
    void* ChildProperties{};
    int32_t Size{};
    int32_t Min{};
    uint8_t pad1[0x48]{};
};

class UClass : public UStruct {
public:
    uint8_t pad2[0x28]{};
    uint64_t ClassCastFlags{};
    UClass* ClassWithin{};
    uint8_t pad3[0x38]{};
    UObject* ClassDefaultObject{};

    UObject* GetFunction(const char* className, const char* funcName) const;
};

inline std::string UObject::GetName() const {
    return Name.ToString();
}

inline UObject* UClass::GetFunction(const char* className, const char* funcName) const {
    for (const UStruct* st = this; st; st = st->Super) {
        if (st->Name.ToString() != className) continue;
        for (UField* field = st->Children; field; field = field->Next) {
            if (field->Name.ToString() == funcName)
                return field;
        }
    }
    return nullptr;
}

}