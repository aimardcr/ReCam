#include "inlinehook.hpp"
#include "tramp_alloc.hpp"

#if defined(__x86_64__)

// x86-64 inline hook; same contract as the arm64 one.

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

// jmp qword ptr [rip+0]; .quad target
constexpr size_t kJumpLen = 14;

void write_abs_jump(uint8_t* dst, uintptr_t destAddr)
{
    dst[0] = 0xFF; dst[1] = 0x25;                  // jmp [rip+disp32]
    dst[2] = dst[3] = dst[4] = dst[5] = 0x00;      // disp32 = 0
    memcpy(dst + 6, &destAddr, sizeof(destAddr));
}

// Length of one instruction, or 0 if it is not a form we are willing to relocate.
size_t insn_len(const uint8_t* p, size_t avail)
{
    if (avail < 1) return 0;
    size_t i = 0;

    // endbr64 is the CET landing pad, the x86 analogue of arm64 BTI.
    if (avail >= 4 && p[0] == 0xF3 && p[1] == 0x0F && p[2] == 0x1E && p[3] == 0xFA)
        return 4;

    // A REX prefix changes no length in the opcodes decoded below.
    if ((p[i] & 0xF0) == 0x40) {
        i++;
        if (i >= avail) return 0;
    }

    const uint8_t op = p[i];

    // push r64 / pop r64  (0x50-0x5F)
    if (op >= 0x50 && op <= 0x5F) return i + 1;

    // The 0x81 / 0x83 group: add/or/adc/sbb/and/sub/xor/cmp r/m64, imm.
    if (op == 0x81 || op == 0x83) {
        if (i + 1 >= avail) return 0;
        const uint8_t modrm = p[i + 1];
        if ((modrm & 0xC0) != 0xC0) return 0;       // not register-direct
        return i + 2 + (op == 0x81 ? 4u : 1u);
    }

    // mov r/m,r and mov r,r/m; register-direct only, so there is no displacement.
    if (op == 0x88 || op == 0x89 || op == 0x8A || op == 0x8B) {
        if (i + 1 >= avail) return 0;
        const uint8_t modrm = p[i + 1];
        if ((modrm & 0xC0) != 0xC0) return 0;
        return i + 2;
    }

    // nop, and the multi-byte 0x0F 0x1F nop form.
    if (op == 0x90) return i + 1;
    if (op == 0x0F && i + 1 < avail && p[i + 1] == 0x1F) {
        if (i + 2 >= avail) return 0;
        const uint8_t modrm = p[i + 2];
        const uint8_t mod = modrm & 0xC0, rm = modrm & 0x07;
        size_t len = i + 3;
        if (mod != 0xC0 && rm == 0x04) len++;                 // SIB
        if (mod == 0x40) len += 1;
        else if (mod == 0x80) len += 4;
        else if (mod == 0x00 && rm == 0x05) return 0;          // RIP-relative
        return len;
    }

    return 0;   // anything else: refuse
}

size_t page_size() { return static_cast<size_t>(sysconf(_SC_PAGESIZE)); }

// The target's text is already executable, so this only adds PROT_WRITE.
bool make_writable(uintptr_t addr, size_t len, size_t* spanOut, uintptr_t* startOut)
{
    const size_t pg = page_size();
    const uintptr_t start = addr & ~(static_cast<uintptr_t>(pg) - 1);
    const size_t span = ((addr + len) - start + pg - 1) & ~(pg - 1);
    if (mprotect(reinterpret_cast<void*>(start), span,
                 PROT_READ | PROT_WRITE | PROT_EXEC) != 0)
        return false;
    *spanOut = span;
    *startOut = start;
    return true;
}

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
        mprotect(reinterpret_cast<void*>(start), span, PROT_READ | PROT_EXEC);
    } else if (!write_via_procmem(addr, src, len)) {
        return false;
    }
    return verify_text(addr, src, len);
    // No explicit i-cache flush: x86 caches are coherent with stores.
}

}  // namespace

const char* hook_result_str(HookResult r)
{
    switch (r) {
        case HookResult::Ok:                    return "ok";
        case HookResult::BadArgs:               return "bad arguments";
        case HookResult::PrologueUnrelocatable: return "prologue has an instruction this hook will not relocate";
        case HookResult::TrampolineAllocFailed: return "could not allocate trampoline";
        case HookResult::TextWriteFailed:       return "could not write to target text";
        case HookResult::NotHooked:             return "not hooked";
    }
    return "?";
}

HookResult hook_install(uintptr_t target, void* replacement, InlineHook* h)
{
    if (!target || !replacement || !h || h->installed) return HookResult::BadArgs;

    const uint8_t* code = reinterpret_cast<const uint8_t*>(target);

    // endbr64 must stay at the entry; patch after it.
    uint32_t patchOff = 0;
    if (code[0] == 0xF3 && code[1] == 0x0F && code[2] == 0x1E && code[3] == 0xFA) {
        patchOff = 4;
        LOGI("hook: endbr64 at entry - preserving it, patching at +4");
    }

    // Decode whole instructions until at least kJumpLen bytes are covered.
    const uint8_t* reloc = code + patchOff;
    size_t covered = 0;
    int count = 0;
    while (covered < kJumpLen) {
        const size_t n = insn_len(reloc + covered, 32);
        if (n == 0) {
            LOGE("hook: cannot relocate instruction %d at +%zu (bytes %02x %02x %02x %02x). "
                 "Refusing to hook rather than split or corrupt it.",
                 count, patchOff + covered,
                 reloc[covered], reloc[covered + 1], reloc[covered + 2], reloc[covered + 3]);
            return HookResult::PrologueUnrelocatable;
        }
        covered += n;
        count++;
        if (covered > sizeof(h->saved)) {
            LOGE("hook: prologue needs %zu bytes, only %zu saved", covered, sizeof(h->saved));
            return HookResult::PrologueUnrelocatable;
        }
    }
    LOGI("hook: relocating %d instruction(s), %zu bytes", count, covered);

    // Carved from our own .text; an anonymous PROT_EXEC mapping would need execmem.
    const size_t trampLen = covered + kJumpLen;
    void* tramp = tramp_alloc(trampLen);
    if (!tramp) return HookResult::TrampolineAllocFailed;

    uint8_t staging[sizeof(h->saved) + kJumpLen];
    memcpy(staging, reloc, covered);
    write_abs_jump(staging + covered, target + patchOff + covered);

    if (!tramp_write(tramp, staging, trampLen)) {
        LOGE("hook: could not write the trampoline.");
        return HookResult::TrampolineAllocFailed;
    }

    memcpy(h->saved, reloc, covered);

    // NOP-pad to the relocated length so no half instruction is left behind.
    uint8_t patch[sizeof(h->saved)];
    memset(patch, 0x90, covered);
    write_abs_jump(patch, reinterpret_cast<uintptr_t>(replacement));

    if (!patch_text(target + patchOff, patch, covered)) {
        LOGE("hook: could not write the jump into the target's text.");
        return HookResult::TextWriteFailed;
    }

    h->target      = target;
    h->trampoline  = reinterpret_cast<uintptr_t>(tramp);
    h->patchOffset = patchOff;
    h->patchLen    = static_cast<uint32_t>(covered);
    h->installed   = true;

    LOGI("hook: installed at %p (+%u, %zu bytes), trampoline %p",
         reinterpret_cast<void*>(target), patchOff, covered, tramp);
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

    // Trampoline deliberately never reclaimed - a thread could still be inside it.
    LOGI("hook: removed from %p (trampoline at %p intentionally retained)",
         reinterpret_cast<void*>(h->target), reinterpret_cast<void*>(h->trampoline));

    h->installed = false;
    return HookResult::Ok;
}

}  // namespace recam

#endif  // __x86_64__
