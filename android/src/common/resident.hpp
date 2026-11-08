// Optional extension in the existing control page; the v2 prefix stays intact.
#pragma once
#include <elf.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace recam {
struct PayloadBuildId {
    uint32_t size = 0;
    uint8_t bytes[32] = {};
    bool operator==(const PayloadBuildId& other) const {
        return size && size <= sizeof(bytes) && size == other.size &&
               memcmp(bytes, other.bytes, size) == 0;
    }
};
inline PayloadBuildId build_id_notes(const void* data, size_t size) {
    const auto* p = static_cast<const uint8_t*>(data);
    while (size >= sizeof(Elf64_Nhdr)) {
        Elf64_Nhdr note;
        memcpy(&note, p, sizeof(note)); p += sizeof(note); size -= sizeof(note);
        const uint64_t name = (uint64_t(note.n_namesz) + 3) & ~uint64_t(3);
        const uint64_t desc = (uint64_t(note.n_descsz) + 3) & ~uint64_t(3);
        if (name > size || desc > size - name) break;
        if (note.n_type == NT_GNU_BUILD_ID && note.n_namesz == 4 &&
            memcmp(p, "GNU", 4) == 0 && note.n_descsz && note.n_descsz <= 32) {
            PayloadBuildId out;
            out.size = note.n_descsz;
            memcpy(out.bytes, p + name, out.size);
            return out;
        }
        p += name + desc; size -= name + desc;
    }
    return {};
}
constexpr uintptr_t kResidentOffset = 64;
constexpr uint64_t kResidentMagic = 0x31544e4544495352ULL; // RSIDENT1
struct ResidentPayload {
    uint64_t magic;
    uint64_t base;
    uint64_t init;
    uint32_t generation;
    PayloadBuildId build;
};
static_assert(kResidentOffset + sizeof(ResidentPayload) <= 4096);
} // namespace recam
