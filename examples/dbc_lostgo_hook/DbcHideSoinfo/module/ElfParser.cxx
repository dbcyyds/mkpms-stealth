module;

#include <link.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>

export module ElfParser;

#ifndef SHT_GNU_HASH
#define SHT_GNU_HASH 0x6ffffff6
#endif

export namespace ElfParser {

    class ElfImage {
    public:

        explicit ElfImage(std::string_view library_name);

        ~ElfImage();

        ElfImage(const ElfImage &) = delete;

        ElfImage &operator=(const ElfImage &) = delete;

        ElfImage(ElfImage &&) = delete;

        ElfImage &operator=(ElfImage &&) = delete;

        ElfW(Addr) findSymbolAddress(std::string_view symbol_name) const;

        template<typename T>
        T findSymbolAddress(std::string_view symbol_name) const {
            return reinterpret_cast<T>(findSymbolAddress(symbol_name));
        }

        std::string_view findSymbolNameByPrefix(std::string_view prefix);

        [[nodiscard]] bool isValid() const { return base_address_ != nullptr && header_ != nullptr; }

        [[nodiscard]] const std::string &getLibraryPath() const { return library_path_; }

    private:
        ElfW(Addr)
        findSymbolOffset(std::string_view symbol_name, uint32_t gnu_hash, uint32_t sysv_hash) const;

        ElfW(Addr) findSymbolByGnuHash(std::string_view symbol_name, uint32_t gnu_hash) const;

        ElfW(Addr) findSymbolBySysvHash(std::string_view symbol_name, uint32_t sysv_hash) const;

        ElfW(Addr) findSymbolByLinearScan(std::string_view symbol_name);

        void buildSymbolCache();

        constexpr static uint32_t calculateSysvHash(std::string_view name);

        constexpr static uint32_t calculateGnuHash(std::string_view name);

        bool findLoadedLibraryInfo(std::string_view library_name);

        std::string library_path_;
        void *base_address_ = nullptr;
        void *map_base_ = nullptr;
        off_t map_size_ = 0;
        ElfW(Ehdr) *header_ = nullptr;
        off_t bias_ = -1;

        ElfW(Sym) *dynsym_ = nullptr;
        const char *dynstr_ = nullptr;
        ElfW(Sym) *symtab_ = nullptr;
        const char *strtab_ = nullptr;
        ElfW(Off) symtab_count_ = 0;

        uint32_t nbucket_ = 0;
        uint32_t *bucket_ = nullptr;
        uint32_t *chain_ = nullptr;


        uint32_t gnu_nbucket_ = 0;
        uint32_t gnu_symindx_ = 0;
        uint32_t gnu_bloom_size_ = 0;
        uint32_t gnu_shift2_ = 0;
        ElfW(Addr) *gnu_bloom_filter_ = nullptr;
        uint32_t *gnu_bucket_ = nullptr;
        uint32_t *gnu_chain_ = nullptr;

        std::unordered_map<std::string_view, const ElfW(Sym) *> symbol_cache_;
    };

    constexpr uint32_t ElfImage::calculateSysvHash(std::string_view name) {
        uint32_t h = 0;
        uint32_t g = 0;
        for (const unsigned char c: name) {
            h = (h << 4) + c;
            g = h & 0xf0000000;
            if (g != 0) {
                h ^= g >> 24;
            }
            h &= ~g;
        }
        return h;
    }

    constexpr uint32_t ElfImage::calculateGnuHash(std::string_view name) {
        uint32_t h = 5381;
        for (const unsigned char c: name) {
            h = (h << 5) + h + c;
        }
        return h;
    }


    template<typename T>
    auto findDirectSymbol(const ElfImage &image, std::string_view symbol_name) {
        auto address = image.findSymbolAddress(symbol_name);

        constexpr bool is_function_pointer =
                std::is_pointer_v<T> && std::is_function_v<std::remove_pointer_t<T>>;

        if constexpr (is_function_pointer) {
            return reinterpret_cast<T>(address);
        } else {
            return reinterpret_cast<T *>(address);
        }
    }

    template<typename T>
    T *resolveSymbolPointer(const ElfImage &image, std::string_view symbol_name) {
        auto *address = reinterpret_cast<T **>(image.findSymbolAddress(symbol_name));
        return (address == nullptr) ? nullptr : *address;
    }

    template<typename T>
    T *pointer_at(void *map_base, ElfW(Off) offset) {
        return reinterpret_cast<T *>(reinterpret_cast<uintptr_t>(map_base) + offset);
    }

    ElfImage::ElfImage(std::string_view library_name) {
        if (!findLoadedLibraryInfo(library_name)) {
            return;
        }

        int fd = open(library_path_.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            return;
        }

        struct stat file_stat{};
        if (fstat(fd, &file_stat) < 0) {
            close(fd);
            return;
        }
        map_size_ = file_stat.st_size;

        map_base_ = mmap(nullptr, map_size_, PROT_READ, MAP_SHARED, fd, 0);
        close(fd);

        if (map_base_ == MAP_FAILED) {
            map_base_ = nullptr;
            return;
        }

        header_ = static_cast<ElfW(Ehdr) *>(map_base_);

        const ElfW(Phdr) *const program_headers = pointer_at<ElfW(Phdr)>(map_base_, header_->e_phoff);
        for (int i = 0; i < header_->e_phnum; ++i) {
            const ElfW(Phdr) *phdr = &program_headers[i];
            if (phdr->p_type == PT_LOAD) {
                bias_ = phdr->p_vaddr - phdr->p_offset;
                break;
            }
        }

        const ElfW(Shdr) *const section_headers = pointer_at<ElfW(Shdr)>(map_base_, header_->e_shoff);
        const ElfW(Shdr) *const shstrtab_header = &section_headers[header_->e_shstrndx];
        const char *const shstrtab = pointer_at<char>(map_base_, shstrtab_header->sh_offset);

        for (int i = 0; i < header_->e_shnum; ++i) {
            const ElfW(Shdr) *shdr = &section_headers[i];
            switch (shdr->sh_type) {
                case SHT_DYNSYM:
                    dynsym_ = pointer_at<ElfW(Sym)>(map_base_, shdr->sh_offset);
                    break;
                case SHT_SYMTAB:
                    symtab_ = pointer_at<ElfW(Sym)>(map_base_, shdr->sh_offset);
                    symtab_count_ = shdr->sh_size / shdr->sh_entsize;
                    break;
                case SHT_STRTAB:
                    if (strcmp(&shstrtab[shdr->sh_name], ".strtab") == 0) {
                        strtab_ = pointer_at<char>(map_base_, shdr->sh_offset);
                    } else if (strcmp(&shstrtab[shdr->sh_name], ".dynstr") == 0) {
                        dynstr_ = pointer_at<char>(map_base_, shdr->sh_offset);
                    }
                    break;
                case SHT_HASH: {
                    const uint32_t *hash_data = pointer_at<uint32_t>(map_base_, shdr->sh_offset);
                    nbucket_ = hash_data[0];
                    bucket_ = const_cast<uint32_t *>(&hash_data[2]);
                    chain_ = const_cast<uint32_t *>(&bucket_[nbucket_]);
                    break;
                }
                case SHT_GNU_HASH: {
                    const uint32_t *gnu_hash_data = pointer_at<uint32_t>(map_base_, shdr->sh_offset);
                    gnu_nbucket_ = gnu_hash_data[0];
                    gnu_symindx_ = gnu_hash_data[1];
                    gnu_bloom_size_ = gnu_hash_data[2];
                    gnu_shift2_ = gnu_hash_data[3];
                    gnu_bloom_filter_ = pointer_at<ElfW(Addr)>(map_base_, shdr->sh_offset + 16);
                    gnu_bucket_ = reinterpret_cast<uint32_t *>(&gnu_bloom_filter_[gnu_bloom_size_]);
                    gnu_chain_ = &gnu_bucket_[gnu_nbucket_];
                    break;
                }
            }
        }
    }

    ElfImage::~ElfImage() {
        if (map_base_) {
            munmap(map_base_, map_size_);
        }
    }

    ElfW(Addr) ElfImage::findSymbolAddress(std::string_view symbol_name) const {
        ElfW(Addr) offset = findSymbolOffset(symbol_name, calculateGnuHash(symbol_name),
                                             calculateSysvHash(symbol_name));

        if (offset > 0 && base_address_ != nullptr) {
            return static_cast<ElfW(Addr)>(reinterpret_cast<uintptr_t>(base_address_) + offset - bias_);
        }

        return 0;
    }

    ElfW(Addr) ElfImage::findSymbolOffset(std::string_view symbol_name, uint32_t gnu_hash,
                                          uint32_t sysv_hash) const {

        if (auto offset = findSymbolByGnuHash(symbol_name, gnu_hash); offset > 0) {
            return offset;
        }
        if (auto offset = findSymbolBySysvHash(symbol_name, sysv_hash); offset > 0) {
            return offset;
        }

        if (auto offset = const_cast<ElfImage *>(this)->findSymbolByLinearScan(symbol_name);
                offset > 0) {
            return offset;
        }

        return 0;
    }

    ElfW(Addr) ElfImage::findSymbolByGnuHash(std::string_view symbol_name, uint32_t gnu_hash) const {
        if (gnu_bloom_filter_ == nullptr) return 0;

        constexpr auto bloom_mask_bits = sizeof(ElfW(Addr)) * 8;
        const ElfW(Addr) bloom_word = gnu_bloom_filter_[(gnu_hash / bloom_mask_bits) % gnu_bloom_size_];
        const ElfW(Addr) mask = (1ULL << (gnu_hash % bloom_mask_bits)) |
                                (1ULL << ((gnu_hash >> gnu_shift2_) % bloom_mask_bits));

        if ((bloom_word & mask) != mask) {
            return 0;
        }

        uint32_t sym_index = gnu_bucket_[gnu_hash % gnu_nbucket_];
        if (sym_index < gnu_symindx_) {
            return 0;
        }

        const uint32_t *chain = &gnu_chain_[sym_index - gnu_symindx_];
        do {

            if (((*chain ^ gnu_hash) >> 1) == 0) {
                const ElfW(Sym) *sym = &dynsym_[sym_index];
                if (symbol_name == &dynstr_[sym->st_name]) {
                    return sym->st_value;
                }
            }
            sym_index++;
        } while ((*chain++ & 1) == 0);

        return 0;
    }

    ElfW(Addr) ElfImage::findSymbolBySysvHash(std::string_view symbol_name, uint32_t sysv_hash) const {
        if (bucket_ == nullptr) return 0;

        for (uint32_t n = bucket_[sysv_hash % nbucket_]; n != 0; n = chain_[n]) {
            const ElfW(Sym) *sym = &dynsym_[n];
            if (symbol_name == &dynstr_[sym->st_name]) {
                return sym->st_value;
            }
        }
        return 0;
    }

    void ElfImage::buildSymbolCache() {
        if (symbol_cache_.empty() && symtab_ != nullptr && strtab_ != nullptr) {
            symbol_cache_.reserve(symtab_count_);
            for (ElfW(Off) i = 0; i < symtab_count_; ++i) {
                const ElfW(Sym) *sym = &symtab_[i];
                const unsigned char type = ELF_ST_TYPE(sym->st_info);
                if ((type == STT_FUNC || type == STT_OBJECT) && sym->st_size > 0) {
                    symbol_cache_.emplace(&strtab_[sym->st_name], sym);
                }
            }
        }
    }

    ElfW(Addr) ElfImage::findSymbolByLinearScan(std::string_view symbol_name) {
        buildSymbolCache();
        if (auto it = symbol_cache_.find(symbol_name); it != symbol_cache_.end()) {
            return it->second->st_value;
        }
        return 0;
    }

    std::string_view ElfImage::findSymbolNameByPrefix(std::string_view prefix) {
        buildSymbolCache();
        for (const auto &pair: symbol_cache_) {
            std::string_view symbol_name = pair.first;
            if (symbol_name.size() >= prefix.size() && symbol_name.substr(0, prefix.size()) == prefix) {
                return symbol_name;
            }
        }
        return "";
    }

    bool ElfImage::findLoadedLibraryInfo(std::string_view library_name) {
        struct LibraryInfo {
            std::string_view name;
            std::string *path_out;
            void **base_addr_out;
            bool found;
        };

        LibraryInfo info = {library_name, &library_path_, &base_address_, false};

        dl_iterate_phdr(
                [](struct dl_phdr_info *phdr_info, size_t, void *data) -> int {
                    auto *lib_info = static_cast<LibraryInfo *>(data);
                    if (phdr_info->dlpi_name && strstr(phdr_info->dlpi_name, lib_info->name.data())) {
                        *lib_info->path_out = phdr_info->dlpi_name;
                        *lib_info->base_addr_out = reinterpret_cast<void *>(phdr_info->dlpi_addr);
                        lib_info->found = true;
                        return 1;
                    }
                    return 0;
                },
                &info);

        return info.found;
    }


}
