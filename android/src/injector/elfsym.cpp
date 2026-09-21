#include "elfsym.hpp"

#include <elf.h>
#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>

#include <KittyUtils.hpp>

extern "C" {
#include "xz.h"
}

namespace recam::elfsym {

const char* const kTargetPrefix =
    "_ZN7android7camera319Camera3OutputStream25returnBufferCheckedLocked";

namespace {

// ILP32 mangles nsecs_t as x and size_t as j, where LP64 uses l and m.
const char* classify_abi(const std::string& mangled)
{
    const std::string tail = mangled.substr(strlen(kTargetPrefix));
    struct { const char* probe; const char* id; } kProbes[] = {
        { "ERKNS0_20camera_stream_bufferEllbi", "V3" },  // sdk 33+  LP64
        { "ERKNS0_20camera_stream_bufferExxbi", "V3" },  // sdk 33+  ILP32
        { "ERKNS0_20camera_stream_bufferElb",   "V2" },  // sdk 31-32 LP64
        { "ERKNS0_20camera_stream_bufferExb",   "V2" },  // sdk 31-32 ILP32
        { "ERK21camera3_stream_bufferlb",       "V1" },  // sdk 30   LP64
        { "ERK21camera3_stream_bufferxb",       "V1" },  // sdk 30   ILP32
    };
    for (const auto& p : kProbes)
        if (tail.rfind(p.probe, 0) == 0) return p.id;
    return "UNKNOWN";
}

struct Mapped {
    const uint8_t* data = nullptr;
    size_t         size = 0;
    int            fd   = -1;

    ~Mapped()
    {
        if (data) munmap(const_cast<uint8_t*>(data), size);
        if (fd >= 0) close(fd);
    }
};

bool map_file(const std::string& path, Mapped* m)
{
    m->fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (m->fd < 0) return false;

    struct stat st{};
    if (fstat(m->fd, &st) != 0 || st.st_size <= 0) return false;

    void* p = mmap(nullptr, static_cast<size_t>(st.st_size), PROT_READ, MAP_PRIVATE, m->fd, 0);
    if (p == MAP_FAILED) return false;

    m->data = static_cast<const uint8_t*>(p);
    m->size = static_cast<size_t>(st.st_size);
    return true;
}

// ---------------------------------------------------------------- xz

// .gnu_debugdata is an .xz stream wrapping the stripped .symtab.
bool xz_inflate(const uint8_t* in, size_t inLen, std::vector<uint8_t>* out)
{
    xz_crc32_init();

    // XZ_SINGLE needs the whole output up front, so grow and retry.
    const size_t kMaxOut = 64u * 1024 * 1024;
    size_t cap = inLen * 16 + (1u << 20);

    while (cap <= kMaxOut) {
        out->assign(cap, 0);

        struct xz_buf b{};
        b.in = const_cast<uint8_t*>(in);
        b.in_pos = 0;
        b.in_size = inLen;
        b.out = out->data();
        b.out_pos = 0;
        b.out_size = cap;

        struct xz_dec* s = xz_dec_init(XZ_SINGLE, 0);
        if (!s) return false;

        enum xz_ret r = xz_dec_run(s, &b);
        xz_dec_end(s);

        if (r == XZ_STREAM_END) {
            out->resize(b.out_pos);
            return true;
        }
        if (r != XZ_BUF_ERROR && r != XZ_MEMLIMIT_ERROR) {
            KITTY_LOGE("elfsym: xz decode failed (%d)", static_cast<int>(r));
            return false;
        }
        cap *= 4;   // output buffer was too small - retry bigger
    }
    KITTY_LOGE("elfsym: .gnu_debugdata larger than the %zu MB cap", kMaxOut >> 20);
    return false;
}

// ---------------------------------------------------------------- ELF walking

// One ELF image held in memory, 32- or 64-bit.
struct Elf {
    const uint8_t* d = nullptr;
    size_t         n = 0;
    bool           is64 = false;

    bool init(const uint8_t* data, size_t size)
    {
        if (size < EI_NIDENT + 16) return false;
        if (memcmp(data, ELFMAG, SELFMAG) != 0) return false;
        d = data; n = size;
        is64 = data[EI_CLASS] == ELFCLASS64;
        return true;
    }

    template <typename Shdr, typename Ehdr>
    const Shdr* sections(uint16_t* count, uint16_t* strndx) const
    {
        const Ehdr* eh = reinterpret_cast<const Ehdr*>(d);
        if (eh->e_shoff == 0 || eh->e_shnum == 0) return nullptr;
        if (eh->e_shoff + static_cast<uint64_t>(eh->e_shnum) * eh->e_shentsize > n) return nullptr;
        *count = eh->e_shnum;
        *strndx = eh->e_shstrndx;
        return reinterpret_cast<const Shdr*>(d + eh->e_shoff);
    }
};

// Scan one symbol table for the first defined function whose name starts with prefix.
template <typename Sym>
bool scan_symtab(const uint8_t* symData, size_t symSize,
                 const char* strTab, size_t strSize,
                 const char* prefix, std::string* name, uint64_t* value, uint64_t* size)
{
    const size_t count = symSize / sizeof(Sym);
    const size_t plen = strlen(prefix);
    const Sym* syms = reinterpret_cast<const Sym*>(symData);

    for (size_t i = 0; i < count; i++) {
        const Sym& s = syms[i];
        if (s.st_name == 0 || s.st_name >= strSize) continue;
        if (s.st_value == 0 || s.st_size == 0) continue;

        const char* nm = strTab + s.st_name;
        if (strncmp(nm, prefix, plen) != 0) continue;

        *name  = nm;
        *value = static_cast<uint64_t>(s.st_value);
        *size  = static_cast<uint64_t>(s.st_size);
        return true;
    }
    return false;
}

template <typename Ehdr, typename Shdr, typename Sym>
bool search_image(const uint8_t* data, size_t size, const char* prefix,
                  std::string* name, uint64_t* value, uint64_t* symSize,
                  std::string* tier, std::vector<uint8_t>* debugdataOut)
{
    Elf e;
    if (!e.init(data, size)) return false;

    uint16_t shnum = 0, shstrndx = 0;
    const Shdr* sh = e.sections<Shdr, Ehdr>(&shnum, &shstrndx);
    if (!sh || shstrndx >= shnum) return false;

    const Shdr& shstr = sh[shstrndx];
    if (shstr.sh_offset + shstr.sh_size > size) return false;
    const char* shnames = reinterpret_cast<const char*>(data + shstr.sh_offset);

    auto sec_name = [&](const Shdr& s) -> const char* {
        return (s.sh_name < shstr.sh_size) ? shnames + s.sh_name : "";
    };

    // Tiers 1 and 2: real symbol tables in this image.
    for (const char* want : { ".dynsym", ".symtab" }) {
        for (uint16_t i = 0; i < shnum; i++) {
            if (strcmp(sec_name(sh[i]), want) != 0) continue;
            if (sh[i].sh_link >= shnum) continue;

            const Shdr& str = sh[sh[i].sh_link];
            if (sh[i].sh_offset + sh[i].sh_size > size) continue;
            if (str.sh_offset + str.sh_size > size) continue;

            if (scan_symtab<Sym>(data + sh[i].sh_offset, sh[i].sh_size,
                                 reinterpret_cast<const char*>(data + str.sh_offset),
                                 str.sh_size, prefix, name, value, symSize)) {
                *tier = want;
                return true;
            }
        }
    }

    // Tier 3: hand the caller the compressed .symtab to recurse into.
    if (debugdataOut) {
        for (uint16_t i = 0; i < shnum; i++) {
            if (strcmp(sec_name(sh[i]), ".gnu_debugdata") != 0) continue;
            if (sh[i].sh_offset + sh[i].sh_size > size) continue;
            debugdataOut->assign(data + sh[i].sh_offset,
                                 data + sh[i].sh_offset + sh[i].sh_size);
            break;
        }
    }
    return false;
}

// Convert a link-time vaddr to a file offset using PT_LOAD. These are NOT the same:
template <typename Ehdr, typename Phdr>
bool vaddr_to_off(const uint8_t* d, size_t n, uint64_t vaddr, uint64_t* off)
{
    const Ehdr* eh = reinterpret_cast<const Ehdr*>(d);
    if (eh->e_phoff + static_cast<uint64_t>(eh->e_phnum) * eh->e_phentsize > n) return false;
    const Phdr* ph = reinterpret_cast<const Phdr*>(d + eh->e_phoff);

    for (uint16_t i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD) continue;
        if (vaddr >= ph[i].p_vaddr && vaddr < ph[i].p_vaddr + ph[i].p_filesz) {
            *off = ph[i].p_offset + (vaddr - ph[i].p_vaddr);
            return true;
        }
    }
    return false;
}

template <typename Ehdr, typename Phdr>
bool first_load_vaddr_impl(const uint8_t* d, size_t n, uint64_t* out)
{
    const Ehdr* eh = reinterpret_cast<const Ehdr*>(d);
    if (eh->e_phoff + static_cast<uint64_t>(eh->e_phnum) * eh->e_phentsize > n) return false;
    const Phdr* ph = reinterpret_cast<const Phdr*>(d + eh->e_phoff);

    bool have = false;
    uint64_t lowest = 0;
    for (uint16_t i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD) continue;
        if (ph[i].p_offset == 0) { *out = ph[i].p_vaddr; return true; }
        if (!have || ph[i].p_vaddr < lowest) { lowest = ph[i].p_vaddr; have = true; }
    }
    if (have) { *out = lowest; return true; }
    return false;
}

}  // namespace

// ---------------------------------------------------------------- public

bool first_load_vaddr(const std::string& path, uint64_t* out)
{
    Mapped m;
    if (!map_file(path, &m)) return false;

    Elf e;
    if (!e.init(m.data, m.size)) return false;

    return e.is64 ? first_load_vaddr_impl<Elf64_Ehdr, Elf64_Phdr>(m.data, m.size, out)
                  : first_load_vaddr_impl<Elf32_Ehdr, Elf32_Phdr>(m.data, m.size, out);
}

bool resolve_in_file(const std::string& path, Target* out)
{
    Mapped m;
    if (!map_file(path, &m)) {
        KITTY_LOGW("elfsym: cannot open %s", path.c_str());
        return false;
    }

    Elf e;
    if (!e.init(m.data, m.size)) return false;

    out->module = path;
    out->is64 = e.is64;

    std::string name, tier;
    uint64_t value = 0, symSize = 0;
    std::vector<uint8_t> debugdata;

    bool found = e.is64
        ? search_image<Elf64_Ehdr, Elf64_Shdr, Elf64_Sym>(
              m.data, m.size, kTargetPrefix, &name, &value, &symSize, &tier, &debugdata)
        : search_image<Elf32_Ehdr, Elf32_Shdr, Elf32_Sym>(
              m.data, m.size, kTargetPrefix, &name, &value, &symSize, &tier, &debugdata);

    // Inner symbols carry outer vaddrs, so offsets resolve against the outer PT_LOADs.
    std::vector<uint8_t> inner;
    if (!found && !debugdata.empty()) {
        if (!xz_inflate(debugdata.data(), debugdata.size(), &inner)) return false;

        Elf ie;
        if (!ie.init(inner.data(), inner.size())) {
            KITTY_LOGE("elfsym: .gnu_debugdata did not decompress to an ELF");
            return false;
        }
        std::vector<uint8_t> none;
        found = ie.is64
            ? search_image<Elf64_Ehdr, Elf64_Shdr, Elf64_Sym>(
                  inner.data(), inner.size(), kTargetPrefix, &name, &value, &symSize, &tier, nullptr)
            : search_image<Elf32_Ehdr, Elf32_Shdr, Elf32_Sym>(
                  inner.data(), inner.size(), kTargetPrefix, &name, &value, &symSize, &tier, nullptr);
        if (found) tier = ".gnu_debugdata";
    }

    if (!found) return false;

    uint64_t off = 0;
    bool haveOff = e.is64 ? vaddr_to_off<Elf64_Ehdr, Elf64_Phdr>(m.data, m.size, value, &off)
                          : vaddr_to_off<Elf32_Ehdr, Elf32_Phdr>(m.data, m.size, value, &off);
    if (!haveOff) {
        KITTY_LOGE("elfsym: vaddr 0x%llx is not inside any PT_LOAD of %s",
                   static_cast<unsigned long long>(value), path.c_str());
        return false;
    }
    if (off + sizeof(out->prologue) > m.size) return false;

    out->symbol  = name;
    out->tier    = tier;
    out->abi     = classify_abi(name);
    out->vaddr   = value;
    out->size    = symSize;
    out->fileOff = off;
    memcpy(out->prologue, m.data + off, sizeof(out->prologue));
    out->ok = true;
    return true;
}

}  // namespace recam::elfsym
