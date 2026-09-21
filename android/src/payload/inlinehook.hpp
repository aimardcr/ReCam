// Minimal arm64 inline hook, purpose-built for one target.

#pragma once

#include <stdint.h>

namespace recam {

enum class HookResult {
    Ok = 0,
    BadArgs,
    PrologueUnrelocatable,  // a PC-relative instruction in the bytes we must move
    TrampolineAllocFailed,
    TextWriteFailed,
    NotHooked,
};

const char* hook_result_str(HookResult r);

struct InlineHook {
    uintptr_t target      = 0;   // function entry
    uintptr_t trampoline  = 0;   // call this to reach the original
    uint32_t  patchOffset = 0;   // 0, or 4 when a BTI landing pad is preserved
    uint32_t  patchLen    = 0;   // bytes overwritten at target + patchOffset
    uint8_t   saved[32]   = {};  // original bytes, for removal
    bool      installed   = false;
};

// On success `h->trampoline` runs the original prologue and returns to the body.
HookResult hook_install(uintptr_t target, void* replacement, InlineHook* h);

// Restore the original bytes. Safe to call on a non-installed hook.
HookResult hook_remove(InlineHook* h);

}  // namespace recam
