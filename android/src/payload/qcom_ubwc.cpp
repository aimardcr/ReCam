#include "qcom_ubwc.hpp"
#include "qcom_layout.hpp"
#include "c2d_abi.hpp"
#include "../common/ring_access.hpp"

#include <android/hardware_buffer.h>
#include <android/log.h>
#include <cerrno>
#include <cstdio>
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
uint32_t g_initEpoch = 0, g_attemptEpoch = 0;
char g_initError[512] = "initialization unavailable";
void* g_supportLibrary = nullptr;
void* g_c2dLibrary = nullptr;
void* g_metadataLibrary = nullptr;
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

// Keep the reason visible even after a cached failure or Stop/Start. The
// callback may be concurrent; this log throttle does not acquire the GPU lock.
FrameSource* rejected(const char* stage, const void* native, int w, int h,
                      int status = 0, int value1 = 0, int value2 = 0) {
    static uint64_t last = 0;
    const auto now = now_ns();
    auto before = __atomic_load_n(&last, __ATOMIC_RELAXED);
    if (before && now - before < 5000000000ull) return nullptr;
    if (!__atomic_compare_exchange_n(&last, &before, now, false,
                                     __ATOMIC_RELAXED, __ATOMIC_RELAXED)) return nullptr;
    const int version = native ? field<int>(native, 0) : -1;
    const int fds = native ? field<int>(native, 4) : -1;
    const int ints = native ? field<int>(native, 8) : -1;
    __android_log_print(ANDROID_LOG_WARN, "ReCam",
        "UBWC rejected: %s; requested=%dx%d status=%d values=%d,%d ABI=%d/%d/%d",
        stage, w, h, status, value1, value2, version, fds, ints);
    if (native && version == 12 && fds == 2 && ints == 23) {
        __android_log_print(ANDROID_LOG_WARN, "ReCam",
            "UBWC handle: magic=0x%x flags=0x%x format=0x%x aligned=%dx%d "
            "requested=%dx%d layers=%u usage=0x%llx size=%u offset=%u metadata_mapped=%d",
            field<uint32_t>(native, 20), field<uint32_t>(native, 24), field<int>(native, 44),
            field<int>(native, 28), field<int>(native, 32), field<int>(native, 36),
            field<int>(native, 40), field<uint32_t>(native, 52),
            static_cast<unsigned long long>(field<uint64_t>(native, 64)),
            field<uint32_t>(native, 72), field<uint32_t>(native, 76),
            field<uint64_t>(native, 92) != 0);
    }
    return nullptr;
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
    dlerror();
    out = reinterpret_cast<T>(dlsym(library, name));
    if (!out) {
        const char* error = dlerror();
        snprintf(g_initError, sizeof(g_initError), "missing symbol %s: %s", name,
                 error ? error : "unavailable");
        __android_log_print(ANDROID_LOG_WARN, "ReCam", "UBWC: %s", g_initError);
    }
    return out != nullptr;
}

using Load = void* (*)(const char*, int);
bool vendor_library(void*& library, Load load, const char* name, const char* path) {
    if (library) return true;
    char sphalError[192] = "SP-HAL loader unavailable";
    if (load) {
        dlerror();
        library = load(name, RTLD_NOW | RTLD_LOCAL);
        if (library) return true;
        const char* error = dlerror();
        snprintf(sphalError, sizeof(sphalError), "%s", error ? error : "unavailable");
    }
    dlerror();
    library = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (library) return true;
    const char* error = dlerror();
    snprintf(g_initError, sizeof(g_initError), "load %s: %s; SP-HAL: %s",
             name, error ? error : "unavailable", sphalError);
    __android_log_print(ANDROID_LOG_WARN, "ReCam", "UBWC: %s", g_initError);
    return false;
}

bool initialize() {
    if (g_available) return true;
    const auto epoch = __atomic_load_n(&g_initEpoch, __ATOMIC_ACQUIRE);
    if (g_tried && g_attemptEpoch == epoch) return false;
    g_attemptEpoch = epoch;
    g_tried = true;
    // Invoked lazily on a real callback, never while the injector has borrowed
    // a cameraserver thread. Keep driver libraries loaded for this image's life.
    if (!g_supportLibrary) g_supportLibrary = dlopen("libvndksupport.so", RTLD_NOW | RTLD_LOCAL);
    auto load = g_supportLibrary ? reinterpret_cast<Load>(
        dlsym(g_supportLibrary, "android_load_sphal_library")) : nullptr;
    if (!vendor_library(g_c2dLibrary, load, "libC2D2.so", "/vendor/lib64/libC2D2.so") ||
        !vendor_library(g_metadataLibrary, load, "libqdMetaData.so", "/vendor/lib64/libqdMetaData.so")) {
        failed("load vendor libraries", -ENOENT); return false;
    }
    void* c2d = g_c2dLibrary;
    void* metadata = g_metadataLibrary;
    const bool ok =
        symbol(c2d, "c2dDriverInit", g_c2d.init) &&
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
    // Mapping requires an initialized driver. A null setup selects the vendor
    // defaults; keep this process-wide driver alive across staging retirement.
    const int rc = g_c2d.init(nullptr);
    if (rc) {
        snprintf(g_initError, sizeof(g_initError), "initialize C2D driver (status=%d)", rc);
        failed("initialize C2D driver", rc);
        return false;
    }
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
    // Query capabilities before submitting work with the new staging surfaces.
    DriverInfo info{};
    const int rc = g_c2d.capabilities(&info);
    if (rc || !(info.capabilities & (1u << 18)) || info.workaround) {
        snprintf(g_initError, sizeof(g_initError), "unsupported C2D capabilities/workarounds (status=%d)", rc);
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

void qcom_ubwc_retry_initialization() {
    // Driver work remains on the next real camera callback, once per Start.
    __atomic_add_fetch(&g_initEpoch, 1u, __ATOMIC_RELEASE);
}

FrameSource* qcom_ubwc_fill(const void* native, int w, int h, int fence,
                          FrameSource* source, FrameSource* fallback) {
    Handle handle;
    Layout l;
    if (!read_handle(native, handle)) return rejected("handle ABI/flags", native, w, h);
    if (!layout(w, h, l)) return rejected("dimensions", native, w, h);
    if (!matches(handle, w, h, l)) return rejected("allocation layout", native, w, h);
    RingWrite guard(&g_access);
    if (!guard) return rejected("converter busy", native, w, h);
    if (g_poisoned) return rejected("disabled after GPU failure", native, w, h);
    if (now_ns() < g_retryAfter) return nullptr;
    if (!initialize()) return rejected(g_initError, native, w, h);
    if (!start_retire_worker()) return rejected("retirement worker", native, w, h);
    __atomic_store_n(&g_retired, 0u, __ATOMIC_RELEASE);
    // QTI stores alternate linear/interlaced geometry in metadata. A missing
    // entry returns -EINVAL; accept only the ordinary progressive allocation.
    // Require an already mapped metadata handle to avoid treating map errors as
    // absent metadata. Camera buffers have been imported by GraphicBufferMapper.
    if (!field<uint64_t>(native, 92)) return rejected("metadata not mapped", native, w, h);
    int interlaced = 0, linear = 0;
    int geometry[2]{};
    int rc = g_metadata(native, 4, &interlaced);
    if ((rc && rc != -EINVAL) || interlaced)
        return rejected("interlace metadata", native, w, h, rc, interlaced);
    rc = g_metadata(native, 0x1000, &linear);
    if ((rc && rc != -EINVAL) || (!rc && linear != kQcomNv12Ubwc))
        return rejected("linear-format metadata", native, w, h, rc, linear);
    rc = g_metadata(native, 0x80, geometry);
    if ((rc && rc != -EINVAL) || (!rc && (geometry[0] != w || geometry[1] != h)))
        return rejected("geometry metadata", native, w, h, rc, geometry[0], geometry[1]);
    if (wait_fence(fence, 30) != 1) return rejected("camera fence", native, w, h);
    if (!prepare(w, h, l)) return rejected("staging preparation", native, w, h);

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
