// Locking a camera buffer for CPU write, from inside cameraserver.

#include "bufferlock.hpp"

#include <android/hardware_buffer.h>
#include <android/log.h>
#include <dlfcn.h>
#include <unistd.h>

#define LOG_TAG "ReCam"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace recam {

namespace {

// ANativeWindowBuffer is platform-internal; it is only ever a void* here.
using PFN_getHardwareBuffer = AHardwareBuffer* (*)(void*);
using PFN_lockPlanes        = int (*)(AHardwareBuffer*, uint64_t, int32_t,
                                      const ARect*, AHardwareBuffer_Planes*);
using PFN_unlock            = int (*)(AHardwareBuffer*, int32_t*);

// system/core/libsystem/include/system/graphics.h, android16-release.
struct AndroidYCbCr {
    void*    y;
    void*    cb;
    void*    cr;
    size_t   ystride;
    size_t   cstride;
    size_t   chroma_step;
    uint32_t reserved[8];
};

// android::Rect derives from ARect plus two empty CRTP bases, so this is its layout.
struct MapperRect { int32_t left, top, right, bottom; };

// GraphicBufferMapper members; `thiz` is the singleton, passed as the implicit x0.
using PFN_lockAsyncYCbCr = int32_t (*)(void* thiz, const void* handle, uint32_t usage,
                                       const MapperRect* bounds, AndroidYCbCr* out,
                                       int fenceFd);
using PFN_mapperUnlock   = int32_t (*)(void* thiz, const void* handle, void* outFence);

PFN_getHardwareBuffer g_getHwb    = nullptr;
PFN_lockPlanes        g_lockPlanes = nullptr;
PFN_unlock            g_unlock     = nullptr;
bool                  g_resolved   = false;
bool                  g_ok         = false;

void**             g_gbmSlot      = nullptr;  // &Singleton<GraphicBufferMapper>::sInstance
PFN_lockAsyncYCbCr g_lockYCbCr    = nullptr;
PFN_mapperUnlock   g_mapperUnlock = nullptr;
bool               g_ycbcrOk      = false;

// 3 << 4, the value BufferUsage and GRALLOC_USAGE_SW_WRITE_OFTEN both use.
constexpr uint32_t kCpuWriteUsage = AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN;

}  // namespace

bool bufferlock_init()
{
    if (g_resolved) return g_ok;
    g_resolved = true;

    g_getHwb = reinterpret_cast<PFN_getHardwareBuffer>(
        dlsym(RTLD_DEFAULT, "ANativeWindowBuffer_getHardwareBuffer"));
    g_lockPlanes = reinterpret_cast<PFN_lockPlanes>(
        dlsym(RTLD_DEFAULT, "AHardwareBuffer_lockPlanes"));
    g_unlock = reinterpret_cast<PFN_unlock>(
        dlsym(RTLD_DEFAULT, "AHardwareBuffer_unlock"));

    g_ok = g_getHwb && g_lockPlanes && g_unlock;
    if (!g_ok) {
        LOGE("bufferlock: missing symbols (getHwb=%p lockPlanes=%p unlock=%p) - "
             "frame substitution disabled",
             reinterpret_cast<void*>(g_getHwb),
             reinterpret_cast<void*>(g_lockPlanes),
             reinterpret_cast<void*>(g_unlock));
    } else {
        LOGI("bufferlock: AHardwareBuffer lock API resolved");
    }

    g_gbmSlot = reinterpret_cast<void**>(dlsym(RTLD_DEFAULT,
        "_ZN7android9SingletonINS_19GraphicBufferMapperEE9sInstanceE"));
    g_lockYCbCr = reinterpret_cast<PFN_lockAsyncYCbCr>(dlsym(RTLD_DEFAULT,
        "_ZN7android19GraphicBufferMapper14lockAsyncYCbCrEPK13native_handle"
        "jRKNS_4RectEP13android_ycbcri"));
    g_mapperUnlock = reinterpret_cast<PFN_mapperUnlock>(dlsym(RTLD_DEFAULT,
        "_ZN7android19GraphicBufferMapper6unlockEPK13native_handle"
        "PNS_4base14unique_fd_implINS4_13DefaultCloserEEE"));

    g_ycbcrOk = g_gbmSlot && g_lockYCbCr && g_mapperUnlock;
    LOGI("bufferlock: android_ycbcr fallback %s (slot=%p lock=%p unlock=%p)",
         g_ycbcrOk ? "available" : "UNAVAILABLE - IMPLEMENTATION_DEFINED will pass through",
         reinterpret_cast<void*>(g_gbmSlot), reinterpret_cast<void*>(g_lockYCbCr),
         reinterpret_cast<void*>(g_mapperUnlock));
    return g_ok;
}

bool bufferlock_have_ycbcr()
{
    return g_ycbcrOk;
}

bool bufferlock_acquire(void* anwBuffer, int32_t fenceFd, LockedBuffer* out)
{
    if (!g_ok || !anwBuffer || !out) return false;

    // NOT interchangeable: an AHardwareBuffer* here would be downcast twice.
    AHardwareBuffer* ahb = g_getHwb(anwBuffer);
    if (!ahb) return false;

    return bufferlock_acquire_ahb(ahb, fenceFd, out);
}

bool bufferlock_acquire_ahb(void* hwBuffer, int32_t fenceFd, LockedBuffer* out)
{
    if (!g_ok || !hwBuffer || !out) return false;
    AHardwareBuffer* ahb = static_cast<AHardwareBuffer*>(hwBuffer);

    // The caller keeps owning its fence, so the lock gets a dup.
    int32_t fence = -1;
    if (fenceFd >= 0) {
        fence = dup(fenceFd);
        if (fence < 0) {
            LOGW("bufferlock: dup(fence %d) failed; not locking", fenceFd);
            return false;
        }
    }

    AHardwareBuffer_Planes planes{};
    // Write-only: a read usage would fault in the pixels we are about to discard.
    const int rc = g_lockPlanes(ahb, AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN,
                                fence, nullptr, &planes);
    if (rc != 0) {
        // lockPlanes owns `fence` even on failure; do not close it here.
        return false;
    }
    if (planes.planeCount < 3) {
        // Not a YUV layout after all - give it straight back untouched.
        int32_t rel = -1;
        g_unlock(ahb, &rel);
        if (rel >= 0) close(rel);
        return false;
    }

    out->handle       = ahb;
    out->nativeHandle = nullptr;
    for (int i = 0; i < 3; i++) {
        out->plane[i].data        = static_cast<uint8_t*>(planes.planes[i].data);
        out->plane[i].rowStride   = static_cast<int32_t>(planes.planes[i].rowStride);
        out->plane[i].pixelStride = static_cast<int32_t>(planes.planes[i].pixelStride);
    }
    return true;
}

bool bufferlock_acquire_handle(const void* nativeHandle, int32_t width, int32_t height,
                               int32_t fenceFd, LockedBuffer* out)
{
    if (!g_ycbcrOk || !nativeHandle || !out || width <= 0 || height <= 0) return false;

    // cameraserver imports every buffer through the mapper, so this is already built.
    void* gbm = *g_gbmSlot;
    if (!gbm) return false;

    int32_t fence = -1;
    if (fenceFd >= 0) {
        fence = dup(fenceFd);
        if (fence < 0) {
            LOGW("bufferlock: dup(fence %d) failed; not locking", fenceFd);
            return false;
        }
    }

    AndroidYCbCr yc{};
    const MapperRect bounds{ 0, 0, width, height };
    // lockAsyncYCbCr wraps the fence in a unique_fd, so it owns it from here on.
    const int32_t rc = g_lockYCbCr(gbm, nativeHandle, kCpuWriteUsage, &bounds, &yc, fence);
    if (rc != 0) return false;

    if (!yc.y || !yc.cb || !yc.cr || !yc.ystride || !yc.cstride || !yc.chroma_step) {
        g_mapperUnlock(gbm, nativeHandle, nullptr);
        return false;
    }

    out->handle       = nullptr;
    out->nativeHandle = nativeHandle;
    out->plane[0] = { static_cast<uint8_t*>(yc.y),  static_cast<int32_t>(yc.ystride), 1 };
    out->plane[1] = { static_cast<uint8_t*>(yc.cb), static_cast<int32_t>(yc.cstride),
                      static_cast<int32_t>(yc.chroma_step) };
    out->plane[2] = { static_cast<uint8_t*>(yc.cr), static_cast<int32_t>(yc.cstride),
                      static_cast<int32_t>(yc.chroma_step) };
    return true;
}

void bufferlock_release(LockedBuffer* b)
{
    if (!b) return;

    if (b->nativeHandle) {
        // A null outFence makes the mapper wait on and close the release fence itself.
        g_mapperUnlock(*g_gbmSlot, b->nativeHandle, nullptr);
        b->nativeHandle = nullptr;
        return;
    }

    if (!g_ok || !b->handle) return;

    int32_t releaseFence = -1;
    g_unlock(static_cast<AHardwareBuffer*>(b->handle), &releaseFence);

    // unlock hands back a fence we own, and nothing waits on it.
    if (releaseFence >= 0) close(releaseFence);

    b->handle = nullptr;
}

}  // namespace recam
