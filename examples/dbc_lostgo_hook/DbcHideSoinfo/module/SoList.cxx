module;

#include <elf.h>
#include <link.h>
#include <cstdint>
#include <sys/cdefs.h>
#include <sys/types.h>

#include <cstddef>
#include <cstdint>
#include <string>

export module SoList;

import ElfParser;
import LinkedList;
import Logger;

export namespace Linker {
    class SoInfoWrapper {
    public:
        inline static size_t field_size_offset = soinfo::get_size_offset();
        inline static size_t field_next_offset = soinfo::get_next_offset();
        inline static size_t field_constructor_called_offset = soinfo::get_constructors_called_offset();
        inline static size_t field_realpath_offset = soinfo::get_realpath_offset();

        inline static const char *(*get_realpath_sym)(SoInfoWrapper *) = nullptr;

        inline static void (*soinfo_free)(SoInfoWrapper *) = nullptr;

        inline static void (*soinfo_unload)(SoInfoWrapper *) = nullptr;

        inline size_t getSize() {
            return *reinterpret_cast<size_t *>(reinterpret_cast<uintptr_t>(this) + field_size_offset);
        }

        inline SoInfoWrapper *getNext() {
            return *reinterpret_cast<SoInfoWrapper **>(reinterpret_cast<uintptr_t>(this) +
                                                       field_next_offset);
        }

        inline bool getConstructorCalled() {
            return *reinterpret_cast<bool *>(reinterpret_cast<uintptr_t>(this) +
                                             field_constructor_called_offset);
        }

        inline const char *getPath() {
            if (get_realpath_sym) return get_realpath_sym(this);

            return (reinterpret_cast<std::string *>(reinterpret_cast<uintptr_t>(this) +
                                                    field_realpath_offset))
                    ->c_str();
        }

        void setSize(size_t size) {
            *reinterpret_cast<size_t *>(reinterpret_cast<uintptr_t>(this) + field_size_offset) = size;
        }

        void setNext(SoInfoWrapper *info) {
            *reinterpret_cast<SoInfoWrapper **>(reinterpret_cast<uintptr_t>(this) + field_next_offset) =
                    info;
        }

        void setConstructorCalled(bool called) {
            *reinterpret_cast<size_t *>(reinterpret_cast<uintptr_t>(this) +
                                        field_constructor_called_offset) = called;
        }
    };

    class ProtectedDataGuard {
    public:
        ProtectedDataGuard() {
            if (ctor != nullptr) (this->*ctor)();
        }

        ~ProtectedDataGuard() {
            if (dtor != nullptr) (this->*dtor)();
        }

        static bool setup(const ElfParser::ElfImage &linker) {
            auto resolve = [&linker](const char *primary, const char *fallback) -> FuncType {
                auto addr = linker.findSymbolAddress(primary);
                if (!addr) {
                    addr = linker.findSymbolAddress(fallback);
                }

                return addr ? MemFunc{.data = {.p = reinterpret_cast<void *>(addr), .adj = 0}}.f
                            : nullptr;
            };

            ctor = resolve("__dl__ZN18ProtectedDataGuardC2Ev", "__dl__ZN18ProtectedDataGuardC1Ev");
            dtor = resolve("__dl__ZN18ProtectedDataGuardD2Ev", "__dl__ZN18ProtectedDataGuardD1Ev");

            return ctor != nullptr && dtor != nullptr;
        }

        ProtectedDataGuard(const ProtectedDataGuard &) = delete;

        void operator=(const ProtectedDataGuard &) = delete;

    private:
        using FuncType = void (ProtectedDataGuard::*)();

        inline static FuncType ctor = nullptr;
        inline static FuncType dtor = nullptr;

        union MemFunc {
            FuncType f;

            struct {
                void *p;
                std::ptrdiff_t adj;
            } data;
        };
    };

    SoInfoWrapper *solinker = nullptr;
    SoInfoWrapper *somain = nullptr;

    uint64_t *g_module_load_counter = nullptr;
    uint64_t *g_module_unload_counter = nullptr;

    const size_t size_block_range = 1024;
    const size_t size_maximal = 0x100000;
    const size_t size_minimal = 0x100;
    const size_t llvm_suffix_length = 25;

    bool findHeuristicOffsets(const std::string &linker_name, SoInfoWrapper *vdso) {
        Logger::Debug("Offsets in header [size, next, constructor_called, realpath]: [{}, {},{},{}]",
                      (void *) SoInfoWrapper::field_size_offset, (void *) SoInfoWrapper::field_next_offset,
                      (void *) SoInfoWrapper::field_constructor_called_offset,
                      (void *) SoInfoWrapper::field_realpath_offset);

        bool size_field_found = false;
        bool next_field_found = false;
        bool constructor_called_field_found = false;

        const size_t linker_realpath_size = linker_name.size();

        for (size_t i = 0; i < size_block_range / sizeof(void *); i++) {
            auto size_of_somain =
                    *reinterpret_cast<size_t *>(reinterpret_cast<uintptr_t>(somain) + i * sizeof(void *));

            if (!size_field_found) {
                if (size_of_somain < size_maximal && size_of_somain > size_minimal) {
                    SoInfoWrapper::field_size_offset = i * sizeof(void *);
                    Logger::Debug("heuristic field_size_offset is {} * {} = {}", i, sizeof(void *),
                         reinterpret_cast<void *>(SoInfoWrapper::field_size_offset));
                    size_field_found = true;
                    continue;
                }
            }
            if (!size_field_found) continue;

            auto field_of_solinker = reinterpret_cast<uintptr_t>(solinker) + i * sizeof(void *);

            if (!next_field_found) {
                auto next_of_solinker = *reinterpret_cast<void **>(field_of_solinker);
                if ((next_of_solinker == somain || (vdso != nullptr && next_of_solinker == vdso))) {
                    SoInfoWrapper::field_next_offset = i * sizeof(void *);
                    Logger::Debug("heuristic field_next_offset is {} * {} = {}", i, sizeof(void *),
                         reinterpret_cast<void *>(SoInfoWrapper::field_next_offset));
                    next_field_found = true;
                    continue;
                }
            }
            if (!next_field_found) continue;

            if (!constructor_called_field_found) {
                auto link_map_head_of_solinker = reinterpret_cast<link_map *>(field_of_solinker);
                // Calculate the number of alignment blocks needed to hold the address,
                // then multiply by the alignment size to get the aligned address.
                // This is an integer-based way to round UP to the next alignment boundary.
                auto index_gap = (sizeof(link_map) + sizeof(void *) - 1) / sizeof(void *);
                uintptr_t look_forward = field_of_solinker + index_gap * sizeof(void *);
                bool *constructor_called_of_solinker = reinterpret_cast<bool *>(look_forward);
                if (*constructor_called_of_solinker && link_map_head_of_solinker->l_addr != 0 &&
                    link_map_head_of_solinker->l_name != nullptr &&
                    strcmp(linker_name.c_str(), link_map_head_of_solinker->l_name) == 0) {
                    SoInfoWrapper::field_constructor_called_offset =
                            look_forward - reinterpret_cast<uintptr_t>(solinker);
                    Logger::Debug("heuristic field_constructor_called_offset is {} [link_map_head:{}]",
                         reinterpret_cast<void *>(SoInfoWrapper::field_constructor_called_offset),
                         reinterpret_cast<void *>(i * sizeof(void *)));
                    constructor_called_field_found = true;
                    i = i + index_gap;
                    continue;
                }
            }
            if (!constructor_called_field_found) continue;

            if (SoInfoWrapper::get_realpath_sym != nullptr) break;

            auto *realpath_of_solinker = reinterpret_cast<std::string *>(field_of_solinker);
            if (realpath_of_solinker->size() == linker_realpath_size) {
                if (strcmp(linker_name.c_str(), realpath_of_solinker->c_str()) == 0) {
                    SoInfoWrapper::field_realpath_offset = i * sizeof(void *);
                    Logger::Debug("heuristic field_realpath_offset is {} * {} = {}", i, sizeof(void *),
                         reinterpret_cast<void *>(SoInfoWrapper::field_realpath_offset));
                    break;
                }
            }
        }

        return size_field_found && next_field_found && constructor_called_field_found;
    }

    bool initialize() {
        ElfParser::ElfImage linker("/linker");
        if (!ProtectedDataGuard::setup(linker)) return false;
        Logger::Debug("found symbol ProtectedDataGuard");

        std::string_view somain_sym_name = linker.findSymbolNameByPrefix("__dl__ZL6somain");
        if (somain_sym_name.empty()) return false;
        Logger::Debug("found symbol name {}", somain_sym_name.data());

        std::string_view soinfo_free_name =
                linker.findSymbolNameByPrefix("__dl__ZL11soinfo_freeP6soinfo");
        if (soinfo_free_name.empty()) return false;
        Logger::Debug("found symbol name {}", soinfo_free_name.data());

        std::string_view soinfo_unload_name =
                linker.findSymbolNameByPrefix("__dl__ZL13soinfo_unloadP6soinfo");
        if (soinfo_unload_name.empty()) return false;
        Logger::Debug("found symbol name {}", soinfo_unload_name.data());

        char llvm_sufix[llvm_suffix_length + 1];

        if (somain_sym_name.length() != strlen("__dl__ZL6somain")) {
            strncpy(llvm_sufix, somain_sym_name.data() + strlen("__dl__ZL6somain"), sizeof(llvm_sufix));
        } else {
            llvm_sufix[0] = '\0';
        }

        char solinker_sym_name[sizeof("__dl__ZL8solinker") + sizeof(llvm_sufix)];
        snprintf(solinker_sym_name, sizeof(solinker_sym_name), "__dl__ZL8solinker%s", llvm_sufix);

        // for SDK < 36 (Android 16), the linker binary is loaded with name solist
        char solist_sym_name[sizeof("__dl__ZL6solist") + sizeof(llvm_sufix)];
        snprintf(solist_sym_name, sizeof(solist_sym_name), "__dl__ZL6solist%s", llvm_sufix);

        char sonext_sym_name[sizeof("__dl__ZL6sonext") + sizeof(llvm_sufix)];
        snprintf(sonext_sym_name, sizeof(sonext_sym_name), "__dl__ZL6sonext%s", llvm_sufix);

        char vdso_sym_name[sizeof("__dl__ZL4vdso") + sizeof(llvm_sufix)];
        snprintf(vdso_sym_name, sizeof(vdso_sym_name), "__dl__ZL4vdso%s", llvm_sufix);

        solinker = ElfParser::resolveSymbolPointer<SoInfoWrapper>(linker, solinker_sym_name);
        if (solinker == nullptr) {
            solinker = ElfParser::resolveSymbolPointer<SoInfoWrapper>(linker, solist_sym_name);
            if (solinker == nullptr) return false;
            Logger::Debug("found symbol solist at {}", static_cast<void *>(solinker));
        } else {
            Logger::Debug("found symbol solinker at {}", static_cast<void *>(solinker));
        }

        auto *vdso = ElfParser::resolveSymbolPointer<SoInfoWrapper>(linker, vdso_sym_name);
        if (vdso != nullptr) Logger::Debug("found symbol vdso at {}", static_cast<void *>( vdso));

        SoInfoWrapper::get_realpath_sym =
                ElfParser::findDirectSymbol<decltype(SoInfoWrapper::get_realpath_sym)>(
                        linker, "__dl__ZNK6soinfo12get_realpathEv");
        if (SoInfoWrapper::get_realpath_sym != nullptr) Logger::Debug("found symbol get_realpath_sym");

        SoInfoWrapper::soinfo_free =
                ElfParser::findDirectSymbol<decltype(SoInfoWrapper::soinfo_free)>(linker, soinfo_free_name);
        if (SoInfoWrapper::soinfo_free == nullptr) return false;
        Logger::Debug("found symbol soinfo_free");

        SoInfoWrapper::soinfo_unload =
                ElfParser::findDirectSymbol<decltype(SoInfoWrapper::soinfo_unload)>(linker,
                                                                                    soinfo_unload_name);
        if (SoInfoWrapper::soinfo_unload == nullptr) return false;
        Logger::Debug("found symbol soinfo_unload");

        g_module_load_counter =
                ElfParser::findDirectSymbol<uint64_t>(linker, "__dl__ZL21g_module_load_counter");
        if (g_module_load_counter != nullptr) Logger::Debug("found symbol g_module_load_counter");

        g_module_unload_counter =
                ElfParser::findDirectSymbol<uint64_t>(linker, "__dl__ZL23g_module_unload_counter");
        if (g_module_unload_counter != nullptr) Logger::Debug("found symbol g_module_unload_counter");

        somain = ElfParser::resolveSymbolPointer<SoInfoWrapper>(linker, somain_sym_name.data());
        if (somain == nullptr) return false;
        Logger::Debug("found symbol somain at {}", static_cast<void *>( somain));

        return findHeuristicOffsets(linker.getLibraryPath(), vdso);
    }


    bool dropSoPath(const char *target_path, bool unload) {
        bool path_found = false;
        if (solinker == nullptr && !initialize()) {
            Logger::Error("failed to initialize solist before dropping paths");
            return path_found;
        }
        for (auto *iter = solinker; iter; iter = iter->getNext()) {
            if (iter->getPath() && strstr(iter->getPath(), target_path)) {
                Linker::ProtectedDataGuard guard;
                auto size = iter->getSize();
                Logger::Debug("dropping solist record for {} [size {}, constructor_called: {}]",
                     iter->getPath(), size, iter->getConstructorCalled());
                if (size > 0) {
                    iter->setSize(0);
                    if (unload) {
                        iter->setConstructorCalled(false);
                        SoInfoWrapper::soinfo_unload(iter);
                        iter->setConstructorCalled(true);
                    } else {
                        SoInfoWrapper::soinfo_free(iter);
                        iter->setSize(size);
                    }
                    path_found = true;
                }
            }
        }
        return path_found;
    }

    void resetCounters(size_t load, size_t unload) {
        if (solinker == nullptr && !initialize()) {
            Logger::Error("failed to initialize solist before resetting counters");
            return;
        }
        if (g_module_load_counter == nullptr || g_module_unload_counter == nullptr) {
            Logger::Debug("g_module counters not defined, skip reseting them");
            return;
        }
        auto loaded_modules = *g_module_load_counter;
        auto unloaded_modules = *g_module_unload_counter;
        if (loaded_modules >= load) {
            *g_module_load_counter = loaded_modules - load;
            Logger::Debug("reset g_module_load_counter: [{} -> {}]", (size_t) loaded_modules,
                 (size_t) *g_module_load_counter);
        }
        if (unloaded_modules >= unload) {
            *g_module_unload_counter = unloaded_modules - unload;
            Logger::Debug("reset g_module_unload_counter: [{} -> {}]", (size_t) unloaded_modules,
                 (size_t) *g_module_unload_counter);
        }
    }

    void SetHide_Soinfo(const char *target_path)
    {
        initialize();
        resetCounters(1, 0);
        dropSoPath(target_path, true);
    }





}
