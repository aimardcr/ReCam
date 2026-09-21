// ELF symbol resolution for the ReCam injector.

#pragma once

#include <stdint.h>
#include <string>
#include <vector>

namespace recam::elfsym {

// Everything after this prefix is the parameter list, which changed in 12 and 13.
extern const char* const kTargetPrefix;

struct Target {
    std::string module;      // path of the module the symbol was found in
    std::string symbol;      // full mangled name
    std::string tier;        // ".dynsym" | ".symtab" | ".gnu_debugdata"
    std::string abi;         // "V1" | "V2" | "V3" | "UNKNOWN"
    uint64_t    vaddr   = 0; // link-time virtual address
    uint64_t    size    = 0;
    uint64_t    fileOff = 0; // where vaddr lives in the file - NOT the same as vaddr
    bool        is64    = false;
    uint8_t     prologue[16] = {};
    bool        ok      = false;
};

// Resolve within one on-disk module. Returns false if the symbol is not there.
bool resolve_in_file(const std::string& path, Target* out);

// p_vaddr of the PT_LOAD covering file offset 0; the load bias is measured from it.
bool first_load_vaddr(const std::string& path, uint64_t* out);

}  // namespace recam::elfsym
