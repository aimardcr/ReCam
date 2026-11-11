#include "qcom_ubwc.hpp"
#include "qcom_layout.hpp"
#include "c2d_abi.hpp"
#include "../common/ring_access.hpp"

#include <android/hardware_buffer.h>
#include <android/log.h>
#include <cerrno>
#include <dlfcn.h>
#include <linux/dma-buf.h>
#include <poll.h>
#include <pthread.h>
#include <semaphore.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

namespace recam {
namespace {

using namespace qcom;
using Allocate = int (*)(const AHardwareBuffer_Desc*, AHardwareBuffer**);
using Release = void (*)(AHardwareBuffer*);
using NativeHandle = const void* (*)(const AHardwareBuffer*);
using GetMetadata = int (*)(const void*, int, void*);

uint32_t g_access = 0;
C2d g_c2d;
Allocate g_allocate = nullptr;
Release g_release = nullptr;
NativeHandle g_nativeHandle = nullptr;
GetMetadata g_metadata = nullptr;
bool g_tried = false, g_available = false, g_poisoned = false;
uint64_t g_retryAfter = 0, g_lastLog = 0;
int g_fence = -1;
bool g_pending = false;
sem_t g_retireWake;
uint32_t g_workerReady = 0, g_retired = 0;
int g_width = 0, g_height = 0;

uint64_t now_ns() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return uint64_t(ts.tv_sec) * 1000000000ull + ts.tv_nsec;
}

void failed(const char* stage, int status) {
    const auto now = now_ns();
    if (now - g_lastLog >= 5000000000ull || !g_lastLog) {
        __android_log_print(ANDROID_LOG_WARN, "ReCam", "UBWC: %s failed (%d)",
                            stage, status);
        g_lastLog = now;
    }
    g_retryAfter = now + 1000000000ull;
}

template <typename T> bool symbol(void* library, const char* name, T& out) {
    out = reinterpret_cast<T>(dlsym(library, name));
    return out != nullptr;
}

bool initialize() {
    if (g_tried) return g_available;
    g_tried = true;
    // Invoked lazily on a real callback, never while the injector has borrowed
    // a cameraserver thread. Keep driver libraries loaded for this image's life.
    void* support = dlopen("libvndksupport.so", RTLD_NOW | RTLD_LOCAL);
    using Load = void* (*)(const char*, int);
    auto load = support ? reinterpret_cast<Load>(dlsym(support, "android_load_sphal_library")) : nullptr;
    void* c2d = load ? load("libC2D2.so", RTLD_NOW | RTLD_LOCAL) : nullptr;
    if (!c2d) c2d = dlopen("/vendor/lib64/libC2D2.so", RTLD_NOW | RTLD_LOCAL);
    void* metadata = load ? load("libqdMetaData.so", RTLD_NOW | RTLD_LOCAL) : nullptr;
    if (!metadata) metadata = dlopen("/vendor/lib64/libqdMetaData.so", RTLD_NOW | RTLD_LOCAL);
    if (!c2d || !metadata) { failed("load vendor libraries", -ENOENT); return false; }
    const bool ok =
        symbol(c2d, "c2dCreateSurface", g_c2d.create) &&
        symbol(c2d, "c2dDestroySurface", g_c2d.destroy) &&
        symbol(c2d, "c2dMapAddr", g_c2d.map) &&
        symbol(c2d, "c2dUnMapAddr", g_c2d.unmap) &&
        symbol(c2d, "c2dDraw", g_c2d.draw) &&
        symbol(c2d, "c2dFlush", g_c2d.flush) &&
        symbol(c2d, "c2dCreateFenceFD", g_c2d.fence) &&
        symbol(c2d, "c2dFinish", g_c2d.finish) &&
        symbol(c2d, "c2dGetDriverCapabilities", g_c2d.capabilities) &&
        symbol(metadata, "getMetaData", g_metadata) &&
        symbol(RTLD_DEFAULT, "AHardwareBuffer_allocate", g_allocate) &&
        symbol(RTLD_DEFAULT, "AHardwareBuffer_release", g_release) &&
        symbol(RTLD_DEFAULT, "AHardwareBuffer_getNativeHandle", g_nativeHandle);
    if (!ok) { failed("resolve C2D/allocator ABI", -ENOSYS); return false; }
    g_available = true;
    return true;
}

bool cpu_sync(int fd, uint64_t flags) {
    dma_buf_sync sync{flags};
    int rc;
    do { rc = ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync); } while (rc < 0 && errno == EINTR);
    if (rc < 0) failed("DMA_BUF_IOCTL_SYNC", -errno);
    return rc == 0;
}

// A bounded wait, including EINTR. Poll never consumes a caller-owned fence.
// 1 means successful completion, 0 pending, -1 invalid/error completion.
int wait_fence(int fd, int milliseconds) {
    if (fd < 0) return 1;
    const uint64_t deadline = now_ns() + uint64_t(milliseconds) * 1000000ull;
    for (;;) {
        const auto now = now_ns();
        int remaining = now >= deadline ? 0 : int((deadline - now + 999999) / 1000000);
        pollfd p{fd, POLLIN, 0};
        const int rc = poll(&p, 1, remaining);
        if (rc < 0 && errno == EINTR && now_ns() < deadline) continue;
        if (rc < 0 || (p.revents & (POLLERR | POLLHUP | POLLNVAL))) return -1;
        return rc > 0 && (p.revents & POLLIN) ? 1 : 0;
    }
}

struct Buffer {
    AHardwareBuffer* allocation = nullptr;
    int fd = -1; // borrowed from allocation
    uint8_t* cpu = nullptr;
    uint32_t bytes = 0;
    void* gpu = nullptr;
    uint32_t surface = 0;

    bool destroy() {
        // Never release backing storage if the driver still owns its surface.
        if (surface) {
            int rc = g_c2d.destroy(surface);
            if (rc) { failed("destroy surface", rc); g_poisoned = true; return false; }
            surface = 0;
        }
        if (gpu) {
            int rc = g_c2d.unmap(gpu);
            if (rc) { failed("unmap GPU buffer", rc); g_poisoned = true; return false; }
            gpu = nullptr;
        }
        if (cpu) { munmap(cpu, bytes); cpu = nullptr; }
        if (allocation) { g_release(allocation); allocation = nullptr; }
        fd = -1; bytes = 0;
        return true;
    }

    bool create(uint32_t size, uint32_t caps, bool compressed, int w, int h, const Layout& l) {
        AHardwareBuffer_Desc desc{};
        desc.width = size; desc.height = 1; desc.layers = 1;
        desc.format = AHARDWAREBUFFER_FORMAT_BLOB;
        desc.usage = AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN | AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN;
        int rc = g_allocate(&desc, &allocation);
        if (rc || !allocation) { failed("allocate staging buffer", rc); return false; }
        Handle info;
        if (!read_handle(g_nativeHandle(allocation), info) || info.format != 0x21 || info.size < size) {
            failed("staging handle ABI", -EINVAL); return false;
        }
        fd = info.fd; bytes = size;
        void* mapping = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (mapping == MAP_FAILED) { failed("map staging buffer", -errno); return false; }
        cpu = static_cast<uint8_t*>(mapping);
        if (!cpu_sync(fd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE)) return false;
        memset(cpu, 0, bytes);
        if (!cpu_sync(fd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE)) return false;
        rc = g_c2d.map(fd, cpu, bytes, 0, kDmaBufMemory, &gpu);
        if (rc || !gpu) { failed("map staging buffer to GPU", rc); return false; }
        // C2D's UBWC pointers refer to metadata starts. It computes data offsets
        // internally; passing gralloc's Y/UV data pointers would offset twice.
        const uint32_t offset = compressed ? l.uvOffset : l.yBytes;
        Surface s{};
        s.format = kNv12 | (compressed ? kUbwc : 0);
        s.width = w; s.height = h;
        s.plane0 = cpu; s.gpu0 = gpu; s.stride0 = l.stride;
        s.plane1 = cpu + offset;
        s.gpu1 = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(gpu) + offset);
        s.stride1 = l.stride;
        rc = g_c2d.create(&surface, caps, kHostYuvPhysical, &s);
        if (rc || !surface) { failed("create C2D surface", rc); return false; }
        return true;
    }
};

Buffer g_linear, g_compressed;

bool finish_pending(int timeoutMs) {
    if (!g_pending) return !g_poisoned;
    // Missing/error fences cannot establish completion: retain bounded staging
    // storage and stop this backend rather than free a live GPU allocation.
    if (g_fence < 0) { g_poisoned = true; return false; }
    int ready = wait_fence(g_fence, timeoutMs);
    if (!ready) return false;
    if (ready < 0) { failed("GPU fence", -EIO); g_poisoned = true; return false; }
    int rc = g_c2d.finish(g_compressed.surface);
    if (rc) { failed("finish completed GPU work", rc); g_poisoned = true; return false; }
    close(g_fence); g_fence = -1; g_pending = false;
    return true;
}

bool destroy_buffers() {
    if (g_poisoned || !finish_pending(0)) return false;
    if (!g_compressed.destroy() || !g_linear.destroy()) return false;
    g_width = g_height = 0;
    return true;
}

void* retire_worker(void*) {
    // The injector can borrow a thread holding a vendor-driver mutex. Perform
    // C2D destruction on this independent thread, never in framehook_remove().
    for (;;) {
        while (sem_wait(&g_retireWake) < 0) {
            if (errno != EINTR) return nullptr;
        }
        while (__atomic_load_n(&g_retired, __ATOMIC_ACQUIRE)) {
            bool retry = true;
            {
                RingWrite guard(&g_access);
                if (guard) {
                    if (!__atomic_load_n(&g_retired, __ATOMIC_ACQUIRE)) break;
                    retry = !g_poisoned && !destroy_buffers();
                    if (!retry) __atomic_store_n(&g_retired, 0u, __ATOMIC_RELEASE);
                }
            }
            if (!retry) break;
            timespec delay{0, 100000000};
            nanosleep(&delay, nullptr);
        }
    }
    return nullptr;
}

bool start_retire_worker() {
    if (__atomic_load_n(&g_workerReady, __ATOMIC_ACQUIRE)) return true;
    if (sem_init(&g_retireWake, 0, 0)) return false;
    pthread_attr_t attr;
    int rc = pthread_attr_init(&attr);
    if (rc) { sem_destroy(&g_retireWake); failed("initialize thread attributes", rc); return false; }
    rc = pthread_attr_setstacksize(&attr, 256 * 1024);
    if (!rc) rc = pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_t thread;
    if (!rc) rc = pthread_create(&thread, &attr, retire_worker, nullptr);
    pthread_attr_destroy(&attr);
    if (rc) { sem_destroy(&g_retireWake); failed("create retirement worker", rc); return false; }
    __atomic_store_n(&g_workerReady, 1u, __ATOMIC_RELEASE);
    return true;
}

bool prepare(int w, int h, const Layout& l) {
    if (!finish_pending(0) || g_poisoned) return false;
    if (g_width == w && g_height == h) return true;
    if (!destroy_buffers()) return false;
    if (!g_linear.create(l.linearBytes, kSource, false, w, h, l) ||
        !g_compressed.create(l.bytes, kTarget, true, w, h, l)) {
        destroy_buffers();
        return false;
    }
    // Surface creation initializes the driver. Query capabilities afterwards.
    DriverInfo info{};
    const int rc = g_c2d.capabilities(&info);
    if (rc || !(info.capabilities & (1u << 18)) || info.workaround) {
        failed("UBWC capability/workaround", rc ? rc : -ENOTSUP);
        destroy_buffers();
        g_available = false;
        return false;
    }
    g_width = w; g_height = h;
    __android_log_print(ANDROID_LOG_INFO, "ReCam", "UBWC: C2D staging ready for %dx%d", w, h);
    return true;
}

} // namespace

FrameSource* qcom_ubwc_fill(const void* native, int w, int h, int fence,
                          FrameSource* source, FrameSource* fallback) {
    Handle handle;
    Layout l;
    if (!read_handle(native, handle) || !layout(w, h, l) || !matches(handle, w, h, l)) return nullptr;
    RingWrite guard(&g_access);
    if (!guard || g_poisoned || now_ns() < g_retryAfter || !initialize()) return nullptr;
    if (!start_retire_worker()) return nullptr;
    __atomic_store_n(&g_retired, 0u, __ATOMIC_RELEASE);
    // QTI stores alternate linear/interlaced geometry in metadata. A missing
    // entry returns -EINVAL; accept only the ordinary progressive allocation.
    // Require an already mapped metadata handle to avoid treating map errors as
    // absent metadata. Camera buffers have been imported by GraphicBufferMapper.
    if (!field<uint64_t>(native, 92)) return nullptr;
    int interlaced = 0, linear = 0;
    int geometry[2]{};
    int rc = g_metadata(native, 4, &interlaced);
    if ((rc && rc != -EINVAL) || interlaced) return nullptr;
    rc = g_metadata(native, 0x1000, &linear);
    if ((rc && rc != -EINVAL) || (!rc && linear != kQcomNv12Ubwc)) return nullptr;
    rc = g_metadata(native, 0x80, geometry);
    if ((rc && rc != -EINVAL) || (!rc && (geometry[0] != w || geometry[1] != h))) return nullptr;
    if (wait_fence(fence, 30) != 1 || !prepare(w, h, l)) return nullptr;

    if (!cpu_sync(g_linear.fd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE)) return nullptr;
    FrameTarget t;
    t.width = w; t.height = h; t.format = 0x23;
    t.y = {g_linear.cpu, int32_t(l.stride), 1};
    t.cb = {g_linear.cpu + l.yBytes, int32_t(l.stride), 2};
    t.cr = {g_linear.cpu + l.yBytes + 1, int32_t(l.stride), 2};
    FrameSource* used = nullptr;
    if (source && source->fill(t)) used = source;
    else if (fallback && fallback->fill(t)) used = fallback;
    if (!cpu_sync(g_linear.fd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE) || !used) return nullptr;

    Object object{};
    object.source = g_linear.surface; object.mask = kCopyMask;
    object.sourceRect = object.targetRect = {0, 0, w << 16, h << 16};
    // From here on, any failure must retain the owned GPU buffers until we have
    // proven completion. No camera allocation has been modified yet.
    g_pending = true;
    rc = g_c2d.draw(g_compressed.surface, 0, nullptr, 0, 0, &object, 1);
    if (rc) { failed("draw", rc); g_poisoned = true; return nullptr; }
    void* timestamp = nullptr;
    rc = g_c2d.flush(g_compressed.surface, &timestamp);
    if (rc) { failed("flush", rc); g_poisoned = true; return nullptr; }
    rc = g_c2d.fence(g_compressed.surface, timestamp, &g_fence);
    if (rc || g_fence < 0) { failed("create completion fence", rc); g_poisoned = true; return nullptr; }
    if (!finish_pending(30)) { failed("completion deadline", -ETIMEDOUT); return nullptr; }

    // Commit only a completed compressed image, including its tile metadata.
    // The camera allocation is never registered with C2D or used as a GPU target.
    void* target = mmap(nullptr, l.bytes, PROT_READ | PROT_WRITE, MAP_SHARED, handle.fd, 0);
    if (target == MAP_FAILED) { failed("map camera allocation", -errno); return nullptr; }
    if (!cpu_sync(g_compressed.fd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ)) {
        munmap(target, l.bytes); return nullptr;
    }
    if (!cpu_sync(handle.fd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE)) {
        cpu_sync(g_compressed.fd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ);
        munmap(target, l.bytes); return nullptr;
    }
    memcpy(target, g_compressed.cpu, l.bytes);
    const bool flushed = cpu_sync(handle.fd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE);
    const bool released = cpu_sync(g_compressed.fd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ);
    munmap(target, l.bytes);
    if (!flushed || !released) {
        g_poisoned = true;
        return nullptr;
    }
    return used;
}

void qcom_ubwc_release() {
    if (__atomic_load_n(&g_workerReady, __ATOMIC_ACQUIRE)) {
        __atomic_store_n(&g_retired, 1u, __ATOMIC_RELEASE);
        sem_post(&g_retireWake);
    }
}

} // namespace recam
