#include "tramp_alloc.hpp"

#include <android/log.h>
#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define LOG_TAG "ReCam"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

extern "C" {
extern uint8_t recam_tramp_arena[];
extern uint8_t recam_tramp_arena_end[];
}

namespace recam {

namespace {

size_t g_used = 0;

size_t page_size() { return static_cast<size_t>(sysconf(_SC_PAGESIZE)); }

// The arena is already in .text, so this only adds PROT_WRITE.
bool protect(void* addr, size_t len, int prot)
{
    const size_t pg = page_size();
    uintptr_t a = reinterpret_cast<uintptr_t>(addr);
    uintptr_t start = a & ~(static_cast<uintptr_t>(pg) - 1);
    size_t span = ((a + len) - start + pg - 1) & ~(pg - 1);
    return mprotect(reinterpret_cast<void*>(start), span, prot) == 0;
}

}  // namespace

void* tramp_alloc(size_t len)
{
    const size_t cap = static_cast<size_t>(recam_tramp_arena_end - recam_tramp_arena);
    len = (len + 15) & ~static_cast<size_t>(15);   // keep 16-byte alignment

    if (g_used + len > cap) {
        LOGE("tramp: arena exhausted (%zu/%zu used, wanted %zu)", g_used, cap, len);
        return nullptr;
    }

    void* p = recam_tramp_arena + g_used;
    g_used += len;
    return p;
}

// FOLL_FORCE bypasses page protection without needing SELinux execmod.
static bool write_via_procmem(void* dst, const void* src, size_t len)
{
    int fd = ::open("/proc/self/mem", O_RDWR | O_CLOEXEC);
    if (fd < 0) return false;
    ssize_t n = ::pwrite(fd, src, len, static_cast<off_t>(reinterpret_cast<uintptr_t>(dst)));
    ::close(fd);
    return n == static_cast<ssize_t>(len);
}

bool tramp_write(void* dst, const void* src, size_t len)
{
    bool ok = false;

    // Try mprotect first: on a device that does grant execmod it is the cheaper path.
    if (protect(dst, len, PROT_READ | PROT_WRITE | PROT_EXEC)) {
        memcpy(dst, src, len);
        protect(dst, len, PROT_READ | PROT_EXEC);
        ok = true;
    } else if (write_via_procmem(dst, src, len)) {
        ok = true;
    }

    if (!ok) {
        LOGE("tramp: could not write the arena by either mprotect or /proc/self/mem");
        return false;
    }

    // Never trust a write into executable memory; read it back.
    if (memcmp(dst, src, len) != 0) {
        LOGE("tramp: write did not stick at %p - executable memory is not writable "
             "by any available means", dst);
        return false;
    }

#if defined(__aarch64__)
    __builtin___clear_cache(static_cast<char*>(dst), static_cast<char*>(dst) + len);
#endif
    return true;
}

}  // namespace recam
