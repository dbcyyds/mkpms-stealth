module;

#include <csetjmp>
#include <csignal>
#include <cstring>
#include <fstream>
#include <link.h>
#include <map>
#include <string>
#include <unistd.h>
#include <sys/mman.h>

export module Memory;

static inline sigjmp_buf g_jmp_env{};

export bool VmRead(void* address, void* result, size_t len) {
    if (reinterpret_cast<uintptr_t>(address) <= 0xFFFFFFFFF) return false;
    if (sigsetjmp(g_jmp_env, 1) == 0) {
        std::memcpy(result, address, len);
        return true;
    }
    return false;
}

export bool VmWrite(void* address, const void* data, size_t len) {
    if (!address || !data || len == 0)
        return false;
    const uintptr_t addr = reinterpret_cast<uintptr_t>(address);
    if (addr <= 0xFFFFFFFFF)
        return false;

    const uintptr_t page = addr & ~static_cast<uintptr_t>(0xFFF);
    ::mprotect(reinterpret_cast<void*>(page), 0x2000, PROT_READ | PROT_WRITE | PROT_EXEC);

    if (sigsetjmp(g_jmp_env, 1) == 0) {
        std::memcpy(address, data, len);
        return true;
    }
    return false;
}

export std::string VmReadString(void* address, int max_len = 0xFF) {
    std::string str;
    str.reserve(max_len);
    for (int i = 0; i < max_len; ++i) {
        char c{};
        if (!VmRead(static_cast<char*>(address) + i, &c, 1) || c == '\0') break;
        str.push_back(c);
    }
    return str;
}

export namespace Memory {
    template<typename T>
    T Read(void* address) {
        T buffer{};
        VmRead(address, &buffer, sizeof(T));
        return buffer;
    }

    template<typename T>
    T Read(uintptr_t address) { return Read<T>(reinterpret_cast<void*>(address)); }

    template<typename T>
    bool Write(void* address, const T& value) {
        return VmWrite(address, &value, sizeof(T));
    }

    template<typename T>
    bool Write(uintptr_t address, const T& value) {
        return Write<T>(reinterpret_cast<void*>(address), value);
    }
}

export int64_t GetModuleBase(const std::string& name) {
    std::map<std::string, int64_t> maps;
    dl_iterate_phdr([](dl_phdr_info* info, size_t, void* data) -> int {
        auto* out = static_cast<std::map<std::string, int64_t>*>(data);
        out->emplace(info->dlpi_name, static_cast<int64_t>(info->dlpi_addr));
        return 0;
    }, &maps);
    for (const auto& [path, base] : maps) {
        if (path.find(name) != std::string::npos) return base;
    }
    return 0;
}

export std::string GetAppDataDir() {
    if (std::ifstream cmdline("/proc/self/cmdline"); cmdline.is_open()) {
        std::string pkg;
        std::getline(cmdline, pkg, '\0');
        if (!pkg.empty()) return "/data/data/" + pkg;
    }
    char buf[512]{};
    if (const ssize_t len = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1); len > 0) {
        std::string path(buf, len);
        if (const size_t pos = path.find_last_of('/'); pos != std::string::npos)
            return "/data/data/" + path.substr(pos + 1);
    }
    return "/data/data/com.tencent.letsgo";
}

export void InitMemoryGuard() {
    signal(SIGSEGV, [](int) { siglongjmp(g_jmp_env, 1); });
}