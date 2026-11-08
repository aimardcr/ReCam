#include "../src/injector/payload_identity.hpp"
#include <cassert>
#include <cstdio>
#include <vector>
int main(int argc, char** argv) {
    std::vector<uint8_t> notes(36);
    Elf64_Nhdr note{4, 20, NT_GNU_BUILD_ID};
    memcpy(notes.data(), &note, sizeof(note));
    memcpy(notes.data() + sizeof(note), "GNU", 4);
    memset(notes.data() + sizeof(note) + 4, 0x42, 20);
    const auto id = recam::build_id_notes(notes.data(), notes.size());
    assert(id.size == 20 && id == id);
    for (size_t n = 0; n < notes.size(); ++n)
        assert(!recam::build_id_notes(notes.data(), n).size);
    note.n_namesz = 0xffffffff;
    memcpy(notes.data(), &note, sizeof(note));
    assert(!recam::build_id_notes(notes.data(), notes.size()).size);
    assert(!(recam::PayloadBuildId{} == recam::PayloadBuildId{}));
    assert(!recam::payload_build_id("/does-not-exist").size);
    if (argc == 2) {
        const auto actual = recam::payload_build_id(argv[1]);
        assert(actual.size && actual == recam::payload_build_id(argv[1]));
        printf("ELF build ID (%u bytes): ", actual.size);
        for (unsigned i = 0; i < actual.size; ++i) printf("%02x", actual.bytes[i]);
        puts("");
    }
    puts("build-ID matching and malformed-note checks passed");
}
