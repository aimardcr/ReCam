#include "inlinehook.hpp"
#include "tramp_alloc.hpp"

#if defined(__aarch64__)

#include <android/log.h>
#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define LOG_TAG "ReCam"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace recam {

namespace {

// x16 is the intra-procedure scratch register, free at a call boundary.
constexpr uint32_t kLdrX16 = 0x58000050;  // LDR X16, #8
constexpr uint32_t kBrX16  = 0xD61F0200;  // BR  X16
constexpr size_t   kJumpLen = 16;         // 4 + 4 + 8

void write_abs_jump(uint8_t* dst, uintptr_t destAddr)
{
    uint32_t insns[2] = { kLdrX16, kBrX16 };
    memcpy(dst, insns, sizeof(insns));
    uint64_t d = destAddr;
    memcpy(dst + 8, &d, sizeof(d));
}

// --- instruction classification -------------------------------------------------

bool is_bti(uint32_t i)
{
    // BTI is HINT #{32,34,36,38}: 0xD503241F / 245F / 249F / 24DF.
    return (i & 0xFFFFFF1Fu) == 0xD503241Fu;
}

// Encodings whose behaviour depends on where they execute.
bool is_pc_relative(uint32_t i)
{
    if ((i & 0x1F000000u) == 0x10000000u) return true;  // ADR / ADRP
    if ((i & 0x7C000000u) == 0x14000000u) return true;  // B / BL
    if ((i & 0xFF000010u) == 0x54000000u) return true;  // B.cond
    if ((i & 0x7E000000u) == 0x34000000u) return true;  // CBZ / CBNZ
    if ((i & 0x7E000000u) == 0x36000000u) return true;  // TBZ / TBNZ
    if ((i & 0x3B000000u) == 0x18000000u) return true;  // LDR (literal)
    return false;
}

// --- memory ---------------------------------------------------------------------

size_t page_size() { return static_cast<size_t>(sysconf(_SC_PAGESIZE)); }

// The target's text is already executable, so this only adds PROT_WRITE.
bool make_writable(uintptr_t addr, size_t len, size_t* pgOut, uintptr_t* startOut)
{
    const size_t pg = page_size();
    const uintptr_t start = addr & ~(static_cast<uintptr_t>(pg) - 1);
    const size_t span = ((addr + len) - start + pg - 1) & ~(pg - 1);

    if (mprotect(reinterpret_cast<void*>(start), span,
                 PROT_READ | PROT_WRITE | PROT_EXEC) != 0)
        return false;

    *pgOut = span;
    *startOut = start;
    return true;
}

void restore_prot(uintptr_t start, size_t span)
{
    mprotect(reinterpret_cast<void*>(start), span, PROT_READ | PROT_EXEC);
}

// /proc/self/mem uses FOLL_FORCE, so it ignores page protection entirely.
bool write_via_procmem(uintptr_t addr, const void* src, size_t len)
{
    int fd = open("/proc/self/mem", O_RDWR | O_CLOEXEC);
    if (fd < 0) return false;
    ssize_t n = pwrite(fd, src, len, static_cast<off_t>(addr));
    close(fd);
    return n == static_cast<ssize_t>(len);
}

bool verify_text(uintptr_t addr, const void* src, size_t len)
{
    // Read back through the pointer the CPU will fetch from.
    if (memcmp(reinterpret_cast<const void*>(addr), src, len) == 0) return true;
    LOGE("hook: text write did not stick at %p", reinterpret_cast<void*>(addr));
    return false;
}

bool patch_text(uintptr_t addr, const void* src, size_t len)
{
    size_t span = 0;
    uintptr_t start = 0;
    if (make_writable(addr, len, &span, &start)) {
        memcpy(reinterpret_cast<void*>(addr), src, len);
        restore_prot(start, span);
    } else if (!write_via_procmem(addr, src, len)) {
        return false;
    }
    __builtin___clear_cache(reinterpret_cast<char*>(addr),
                            reinterpret_cast<char*>(addr + len));
    return verify_text(addr, src, len);
}

}  // namespace

const char* hook_result_str(HookResult r)
{
    switch (r) {
        case HookResult::Ok:                    return "ok";
        case HookResult::BadArgs:               return "bad arguments";
        case HookResult::PrologueUnrelocatable: return "prologue contains a PC-relative instruction";
        case HookResult::TrampolineAllocFailed: return "could not allocate trampoline";
        case HookResult::TextWriteFailed:       return "could not write to target text";
        case HookResult::NotHooked:             return "not hooked";
    }
    return "?";
}

HookResult hook_install(uintptr_t target, void* replacement, InlineHook* h)
{
    if (!target || !replacement || !h || h->installed) return HookResult::BadArgs;

    const uint32_t* code = reinterpret_cast<const uint32_t*>(target);

    // A BTI landing pad must stay at the entry; patch after it.
    uint32_t patchOff = 0;
    if (is_bti(code[0])) {
        patchOff = 4;
        LOGI("hook: BTI landing pad at entry (0x%08x) - preserving it, patching at +4",
             code[0]);
    }

    // The 4 instructions we are about to relocate must be position-independent.
    const uint32_t* reloc = reinterpret_cast<const uint32_t*>(target + patchOff);
    for (int i = 0; i < 4; i++) {
        if (is_pc_relative(reloc[i])) {
            LOGE("hook: instruction %d at +%u is PC-relative (0x%08x). "
                 "Refusing to hook rather than corrupt it.",
                 i, patchOff + i * 4, reloc[i]);
            return HookResult::PrologueUnrelocatable;
        }
    }

    // Trampoline: the relocated prologue, then an absolute jump back past the patch.
    const size_t trampLen = kJumpLen * 2;
    void* tramp = tramp_alloc(trampLen);
    if (!tramp) return HookResult::TrampolineAllocFailed;

    uint8_t staging[kJumpLen * 2];
    memcpy(staging, reloc, kJumpLen);                                 // 4 original insns
    write_abs_jump(staging + kJumpLen, target + patchOff + kJumpLen); // back to the body

    if (!tramp_write(tramp, staging, trampLen)) {
        LOGE("hook: could not write the trampoline.");
        return HookResult::TrampolineAllocFailed;
    }

    // Save what we are about to overwrite, so removal is exact.
    memcpy(h->saved, reinterpret_cast<void*>(target + patchOff), kJumpLen);

    uint8_t jump[kJumpLen];
    write_abs_jump(jump, reinterpret_cast<uintptr_t>(replacement));

    if (!patch_text(target + patchOff, jump, kJumpLen)) {
        LOGE("hook: could not write the jump into the target's text.");
        return HookResult::TextWriteFailed;
    }

    h->target      = target;
    h->trampoline  = reinterpret_cast<uintptr_t>(tramp);
    h->patchOffset = patchOff;
    h->patchLen    = kJumpLen;
    h->installed   = true;

    LOGI("hook: installed at %p (+%u), trampoline %p",
         reinterpret_cast<void*>(target), patchOff, tramp);
    return HookResult::Ok;
}

HookResult hook_remove(InlineHook* h)
{
    if (!h || !h->installed) return HookResult::NotHooked;

    if (!patch_text(h->target + h->patchOffset, h->saved, h->patchLen)) {
        LOGE("hook: could not restore the original bytes at %p",
             reinterpret_cast<void*>(h->target + h->patchOffset));
        return HookResult::TextWriteFailed;
    }

    // Never reclaimed: a thread could still be inside it.
    LOGI("hook: removed from %p (trampoline at %p intentionally retained)",
         reinterpret_cast<void*>(h->target), reinterpret_cast<void*>(h->trampoline));

    h->installed = false;
    return HookResult::Ok;
}

}  // namespace recam

#endif  // __aarch64__
