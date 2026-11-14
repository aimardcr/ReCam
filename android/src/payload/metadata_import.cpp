#include "metadata_import.hpp"
#include "../common/resident.hpp"

#include <dlfcn.h>
#include <elf.h>
#include <cstring>
#include <sys/mman.h>
#include <unistd.h>

namespace recam {
namespace {

struct Image {
    uintptr_t base;
    const Elf64_Phdr* ph;
    size_t count;

    bool contains(uintptr_t address, size_t bytes, unsigned flags = PF_R) const {
        if (!bytes || address > UINTPTR_MAX - bytes) return false;
        for (size_t i = 0; i < count; ++i) {
            const auto& p = ph[i];
            if (p.p_type != PT_LOAD || (p.p_flags & flags) != flags) continue;
            if (p.p_vaddr > UINTPTR_MAX - base) continue;
            const uintptr_t start = base + p.p_vaddr;
            if (p.p_memsz > UINTPTR_MAX - start) continue;
            if (address >= start && address + bytes <= start + p.p_memsz) return true;
        }
        return false;
    }

    uintptr_t pointer(uintptr_t value, size_t bytes) const {
        if (contains(value, bytes)) return value;
        if (value <= UINTPTR_MAX - base && contains(base + value, bytes)) return base + value;
        return 0;
    }

    bool build_id(const uint8_t* expected, size_t size) const {
        if (!expected || !size || size > sizeof(PayloadBuildId{}.bytes)) return false;
        for (size_t i = 0; i < count; ++i) {
            const auto& segment = ph[i];
            if (segment.p_type != PT_NOTE || segment.p_memsz > 65536) continue;
            const auto notes = pointer(segment.p_vaddr, segment.p_memsz);
            if (!notes) continue;
            const auto id = build_id_notes(reinterpret_cast<const void*>(notes), segment.p_memsz);
            if (id.size) return id.size == size && !memcmp(id.bytes, expected, size);
        }
        return false;
    }

    int protection(uintptr_t address, size_t pageSize) const {
        int flags = 0;
        for (size_t i = 0; i < count; ++i) {
            const auto& p = ph[i];
            if (p.p_type == PT_LOAD && address >= base + p.p_vaddr &&
                address - (base + p.p_vaddr) < p.p_memsz) {
                if (p.p_flags & PF_R) flags |= PROT_READ;
                if (p.p_flags & PF_W) flags |= PROT_WRITE;
                if (p.p_flags & PF_X) flags |= PROT_EXEC;
            }
        }
        for (size_t i = 0; i < count; ++i) {
            const auto& p = ph[i];
            if (p.p_type != PT_GNU_RELRO) continue;
            const auto begin = (base + p.p_vaddr) & ~(pageSize - 1);
            const auto end = (base + p.p_vaddr + p.p_memsz + pageSize - 1) & ~(pageSize - 1);
            if (address >= begin && address < end) flags &= ~PROT_WRITE;
        }
        return flags;
    }
};

bool find_slot(const Image& image, uintptr_t*& slot,
               const char* wanted = "find_camera_metadata_ro_entry") {
    const size_t wantedSize = strlen(wanted) + 1;
    if (wantedSize < 2 || wantedSize > 256) return false;
    const Elf64_Dyn* dynamic = nullptr;
    size_t count = 0;
    for (size_t i = 0; i < image.count; ++i) {
        const auto& p = image.ph[i];
        if (p.p_type != PT_DYNAMIC) continue;
        auto address = image.pointer(p.p_vaddr, p.p_memsz);
        if (!address || p.p_memsz / sizeof(Elf64_Dyn) > 4096) return false;
        dynamic = reinterpret_cast<const Elf64_Dyn*>(address);
        count = p.p_memsz / sizeof(Elf64_Dyn);
    }
    if (!dynamic) return false;
    uintptr_t strings = 0, symbols = 0, rela = 0, plt = 0;
    size_t stringBytes = 0, relaBytes = 0, pltBytes = 0, symbolBytes = 0;
    bool relaPlt = false;
    for (size_t i = 0; i < count && dynamic[i].d_tag != DT_NULL; ++i) {
        const auto value = dynamic[i].d_un.d_val;
        switch (dynamic[i].d_tag) {
            case DT_STRTAB: strings = value; break;
            case DT_STRSZ: stringBytes = value; break;
            case DT_SYMTAB: symbols = value; break;
            case DT_SYMENT: symbolBytes = value; break;
            case DT_RELA: rela = value; break;
            case DT_RELASZ: relaBytes = value; break;
            case DT_JMPREL: plt = value; break;
            case DT_PLTRELSZ: pltBytes = value; break;
            case DT_PLTREL: relaPlt = value == DT_RELA; break;
        }
    }
    strings = image.pointer(strings, stringBytes);
    symbols = image.pointer(symbols, sizeof(Elf64_Sym));
    if (!strings || !symbols || symbolBytes != sizeof(Elf64_Sym)) return false;
    auto scan = [&](uintptr_t address, size_t bytes) {
        if (!bytes) return true;
        if (bytes % sizeof(Elf64_Rela) || bytes / sizeof(Elf64_Rela) > 1000000) return false;
        address = image.pointer(address, bytes);
        if (!address) return false;
        const auto* entries = reinterpret_cast<const Elf64_Rela*>(address);
        for (size_t i = 0; i < bytes / sizeof(Elf64_Rela); ++i) {
            const auto& r = entries[i];
            const auto type = ELF64_R_TYPE(r.r_info);
#if defined(__aarch64__)
            if (type != R_AARCH64_JUMP_SLOT && type != R_AARCH64_GLOB_DAT) continue;
#elif defined(__x86_64__)
            if (type != R_X86_64_JUMP_SLOT && type != R_X86_64_GLOB_DAT) continue;
#else
#error Unsupported import ABI
#endif
            const uint64_t offset = uint64_t(ELF64_R_SYM(r.r_info)) * sizeof(Elf64_Sym);
            if (offset > UINTPTR_MAX - symbols) return false;
            const auto symbolAddress = symbols + offset;
            if (!image.contains(symbolAddress, sizeof(Elf64_Sym))) return false;
            const auto* symbol = reinterpret_cast<const Elf64_Sym*>(symbolAddress);
            if (symbol->st_name >= stringBytes) return false;
            const char* name = reinterpret_cast<const char*>(strings + symbol->st_name);
            if (stringBytes - symbol->st_name < wantedSize ||
                memcmp(name, wanted, wantedSize) != 0) continue;
            const auto candidate = image.pointer(r.r_offset, sizeof(uintptr_t));
            if (!candidate || candidate % alignof(uintptr_t) || r.r_addend) return false;
            if (slot && reinterpret_cast<uintptr_t>(slot) != candidate) return false;
            slot = reinterpret_cast<uintptr_t*>(candidate);
        }
        return true;
    };
    return (!pltBytes || relaPlt) && scan(plt, pltBytes) && scan(rela, relaBytes) && slot;
}

bool exchange_slot(MetadataImport& hook, uintptr_t expected, uintptr_t replacement) {
    const long size = sysconf(_SC_PAGESIZE);
    if (size <= 0 || (size & (size - 1))) return false;
    void* page = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(hook.slot) & ~(uintptr_t(size) - 1));
    if (mprotect(page, size, hook.protection | PROT_WRITE) != 0) return false;
    const bool changed = __atomic_compare_exchange_n(hook.slot, &expected, replacement, false,
                                                      __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    // A failed permission restoration must not leave ownership ambiguous.
    if (mprotect(page, size, hook.protection) != 0) {
        if (changed) {
            uintptr_t current = replacement;
            __atomic_compare_exchange_n(hook.slot, &current, expected, false,
                                         __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
        }
        mprotect(page, size, hook.protection);
        return false;
    }
    return changed;
}

} // namespace

bool metadata_import_build_id(uintptr_t cameraFunction, const uint8_t* expected, size_t size) {
    if (!expected || !size || size > sizeof(PayloadBuildId{}.bytes)) return false;
    Dl_info info{};
    if (!dladdr(reinterpret_cast<void*>(cameraFunction), &info) || !info.dli_fbase ||
        !info.dli_fname || !strstr(info.dli_fname, "/libcameraservice.so")) return false;
    const auto base = reinterpret_cast<uintptr_t>(info.dli_fbase);
    const auto* eh = reinterpret_cast<const Elf64_Ehdr*>(base);
    if (memcmp(eh->e_ident, ELFMAG, SELFMAG) || eh->e_ident[EI_CLASS] != ELFCLASS64 ||
        eh->e_type != ET_DYN || eh->e_phentsize != sizeof(Elf64_Phdr) ||
        !eh->e_phnum || eh->e_phnum > 128 || eh->e_phoff > 4096) return false;
    Image image{base, reinterpret_cast<const Elf64_Phdr*>(base + eh->e_phoff), eh->e_phnum};
    return image.build_id(expected, size);
}

bool metadata_import_install(uintptr_t cameraFunction, uintptr_t replacement, MetadataImport& hook,
                             const char* name) {
    if (hook.installed) return true;
    Dl_info info{};
    if (!dladdr(reinterpret_cast<void*>(cameraFunction), &info) || !info.dli_fbase ||
        !info.dli_fname || !strstr(info.dli_fname, "/libcameraservice.so")) return false;
    const auto base = reinterpret_cast<uintptr_t>(info.dli_fbase);
    const auto* eh = reinterpret_cast<const Elf64_Ehdr*>(base);
    if (memcmp(eh->e_ident, ELFMAG, SELFMAG) || eh->e_ident[EI_CLASS] != ELFCLASS64 ||
        eh->e_type != ET_DYN || eh->e_phentsize != sizeof(Elf64_Phdr) ||
        !eh->e_phnum || eh->e_phnum > 128 || eh->e_phoff > 4096) return false;
    Image image{base, reinterpret_cast<const Elf64_Phdr*>(base + eh->e_phoff), eh->e_phnum};
    uintptr_t* slot = nullptr;
    if (!find_slot(image, slot, name)) return false;
    const auto original = reinterpret_cast<uintptr_t>(dlsym(RTLD_DEFAULT, name));
    if (!original || __atomic_load_n(slot, __ATOMIC_SEQ_CST) != original) return false;
    const long pageSize = sysconf(_SC_PAGESIZE);
    if (pageSize <= 0 || (pageSize & (pageSize - 1))) return false;
    const int protection = image.protection(reinterpret_cast<uintptr_t>(slot), pageSize);
    if (!(protection & PROT_READ) || (protection & PROT_EXEC)) return false;
    hook.slot = slot;
    hook.protection = protection;
    hook.replacement = replacement;
    __atomic_store_n(&hook.original, original, __ATOMIC_RELEASE);
    hook.installed = exchange_slot(hook, original, replacement);
    return hook.installed;
}

bool metadata_import_remove(MetadataImport& hook) {
    if (!hook.installed) return true;
    if (!exchange_slot(hook, hook.replacement, hook.original)) return false;
    hook.installed = false;
    return true;
}

} // namespace recam
