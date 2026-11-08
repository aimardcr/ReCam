#pragma once
#include "../common/resident.hpp"
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace recam {
// Read only bounded PT_NOTE records; no dependency on debug/section tables.
inline PayloadBuildId payload_build_id(const char* path) {
    const int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return {};
    struct Close { int fd; ~Close() { close(fd); } } guard{fd};
    struct stat st{};
    Elf64_Ehdr eh{};
    if (fstat(fd, &st) || pread(fd, &eh, sizeof(eh), 0) != sizeof(eh) ||
        memcmp(eh.e_ident, ELFMAG, SELFMAG) || eh.e_ident[EI_CLASS] != ELFCLASS64 ||
        eh.e_ident[EI_DATA] != ELFDATA2LSB || eh.e_phentsize != sizeof(Elf64_Phdr) ||
        eh.e_phnum > 1024 || eh.e_phoff > uint64_t(st.st_size) ||
        uint64_t(eh.e_phnum) * sizeof(Elf64_Phdr) > uint64_t(st.st_size) - eh.e_phoff) return {};
    for (unsigned i = 0; i < eh.e_phnum; ++i) {
        Elf64_Phdr ph{};
        if (pread(fd, &ph, sizeof(ph), eh.e_phoff + i * sizeof(ph)) != sizeof(ph)) return {};
        if (ph.p_type != PT_NOTE || ph.p_filesz > 65536 ||
            ph.p_offset > uint64_t(st.st_size) || ph.p_filesz > uint64_t(st.st_size) - ph.p_offset) continue;
        uint8_t notes[65536];
        if (pread(fd, notes, ph.p_filesz, ph.p_offset) != ssize_t(ph.p_filesz)) return {};
        const auto id = build_id_notes(notes, ph.p_filesz);
        if (id.size) return id;
    }
    return {};
}
} // namespace recam
