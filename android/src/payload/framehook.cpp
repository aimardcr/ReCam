// The hook on Camera3OutputStream::returnBufferCheckedLocked.

#include <android/log.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <initializer_list>
#include "../common/ring_access.hpp"

#include "framehook.hpp"
#include "inlinehook.hpp"
#include "bufferlock.hpp"
#include "qcom_ubwc.hpp"
#include "qcom_layout.hpp"
#include "framesource.hpp"
#include "shmsource.hpp"

#define LOG_TAG "ReCam"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace recam {

// UBWC has its own converter; it must never reach the linear CPU-write path.
bool framehook_format_supported(int32_t f)
{
    if (f == 0x23 || f == 0x32315659 || f == kQcomNv12Ubwc)
        return true;
    return f == 0x22 && bufferlock_have_ycbcr();  // IMPLEMENTATION_DEFINED
}

namespace {

// --- struct offsets, all verified in docs/01-ground-truth.md ---------------------

// camera_stream_buffer, Camera3StreamInterface.h:74-80.
struct StreamBufferView {
    const void* stream;         // +0   camera_stream*
    void**      buffer;         // +8   buffer_handle_t*  == &anwBuffer->handle
    int32_t     status;         // +16
    int32_t     acquireFence;   // +20
    int32_t     releaseFence;   // +24
};

// camera_stream: these four fields sit at the same offsets on every release.
struct StreamHead {
    int32_t  streamType;  // +0
    uint32_t width;       // +4
    uint32_t height;      // +8
    int32_t  format;      // +12
};

// ANativeWindowBuffer, nativebase.h:66-103; byte-identical on Android 11, 13 and 16.
constexpr size_t kAnwHandleOff = 96;   // LP64
constexpr size_t kAnwWidthOff  = 56;
constexpr size_t kAnwHeightOff = 60;
constexpr size_t kAnwStrideOff = 64;
constexpr size_t kAnwFormatOff = 68;
constexpr size_t kAnwUsageOff  = 104;

template <typename T>
T load(const void* base, size_t off)
{
    T v{};
    memcpy(&v, static_cast<const uint8_t*>(base) + off, sizeof(T));
    return v;
}

// --- state ----------------------------------------------------------------------

using TargetFn = int64_t (*)(void*, const void*, int64_t, int64_t,
                             int64_t, int64_t, int64_t, int64_t);

InlineHook   g_hook;
TargetFn     g_orig = nullptr;
uint64_t     g_frames = 0;
uint64_t     g_substituted = 0;
uint64_t     g_passedThrough = 0;
FrameSource* g_shm  = nullptr;   // real content, when a feeder is running
FrameSource* g_bars = nullptr;   // fallback, so an un-fed hook is still visibly live

// Sequentially consistent admission/counting ensures a callback either holds a
// reference before retirement observes zero, or observes disabled substitution.
uint32_t g_callbacks = 0, g_enabled = 0;
enum : uint32_t { Running, Retiring, Reclaiming, Dormant, Preparing };
uint32_t g_lifecycle = Running;

void reclaim_ring_if_idle()
{
    for (;;) {
        if (__atomic_load_n(&g_callbacks, __ATOMIC_SEQ_CST) != 0) return;
        uint32_t expected = Retiring;
        if (!__atomic_compare_exchange_n(&g_lifecycle, &expected, Reclaiming, false,
                                          __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) return;
        if (__atomic_load_n(&g_callbacks, __ATOMIC_SEQ_CST) != 0) {
            __atomic_store_n(&g_lifecycle, Retiring, __ATOMIC_SEQ_CST);
            continue; // closes the last-disabled-callback election race
        }
        qcom_ubwc_release();
        shmsource_destroy();
        __atomic_store_n(&g_lifecycle, Dormant, __ATOMIC_SEQ_CST);
        return;
    }
}

struct CallbackScope {
    bool substitute;
    CallbackScope() {
        __atomic_add_fetch(&g_callbacks, 1, __ATOMIC_SEQ_CST);
        substitute = __atomic_load_n(&g_enabled, __ATOMIC_SEQ_CST) != 0;
    }
    ~CallbackScope() {
        if (__atomic_sub_fetch(&g_callbacks, 1, __ATOMIC_SEQ_CST) == 0)
            reclaim_ring_if_idle();
    }
};



// A bounded, nonblocking per-stream log cache. A newly observed layout is
// logged immediately; repeating frames are summarized at most every five seconds.
struct StreamLog {
    const void* stream = nullptr;
    uint64_t signature = 0;
    uint64_t lastNs = 0;
    uint64_t frames = 0;
};
StreamLog g_streamLogs[32]{};
uint32_t g_logAccess = 0;

bool log_stream(const void* stream, const char* reason, int w, int h, int format,
                const FrameTarget* t = nullptr) {
    RingWrite guard(&g_logAccess);
    if (!guard) return false;
    uint64_t signature = reinterpret_cast<uintptr_t>(reason);
    const auto mix = [&](uint64_t v) { signature = (signature ^ v) * 1099511628211ull; };
    mix(static_cast<uint32_t>(w)); mix(static_cast<uint32_t>(h)); mix(static_cast<uint32_t>(format));
    if (t) {
        for (auto p : {t->y, t->cb, t->cr}) { mix(p.rowStride); mix(p.pixelStride); }
        mix(reinterpret_cast<uintptr_t>(t->cb.data) - reinterpret_cast<uintptr_t>(t->cr.data));
    }
    StreamLog* entry = nullptr;
    for (auto& e : g_streamLogs) {
        if (e.frames && e.stream == stream && e.signature == signature) { entry = &e; break; }
        if (!e.frames && !entry) entry = &e;
    }
    if (!entry) {
        entry = &g_streamLogs[0];
        for (auto& e : g_streamLogs) if (e.lastNs < entry->lastNs) entry = &e;
        entry->frames = 0;
    }
    timespec ts{}; clock_gettime(CLOCK_MONOTONIC, &ts);
    const uint64_t now = static_cast<uint64_t>(ts.tv_sec) * 1000000000ull + ts.tv_nsec;
    const bool first = entry->frames == 0;
    entry->stream = stream; entry->signature = signature; ++entry->frames;
    if (!first && now - entry->lastNs < 5000000000ull) return false;
    entry->lastNs = now;
    return true;
}

const char* fmt_name(int32_t f)
{
    switch (f) {
        case 0x00000021: return "BLOB";
        case 0x00000022: return "IMPL_DEF";
        case 0x00000023: return "YCbCr_420_888";
        case 0x00000020: return "RAW16";
        case 0x00000025: return "RAW10";
        case 0x00000026: return "RAW12";
        case 0x32315659: return "YV12";
        case 0x00000011: return "YCrCb_420_SP";
        case 0x00000001: return "RGBA_8888";
        case 0x7fa30c06: return "QCOM_NV12_UBWC";
        default:         return "?";
    }
}

// --- the hook --------------------------------------------------------------------

extern "C" int64_t recam_on_return_buffer(void* thiz, const void* buffer,
                                          int64_t a2, int64_t a3, int64_t a4,
                                          int64_t a5, int64_t a6, int64_t a7)
{
    // Includes the original call: shutdown may have interrupted this very thread.
    CallbackScope scope;
    // HAL result callback thread: a fault here takes cameraserver down.
    if (scope.substitute && buffer) {
        StreamBufferView sb{};
        memcpy(&sb, buffer, sizeof(sb));

        uint32_t sw = 0, sh = 0;
        int32_t  sfmt = 0, stype = 0;
        if (sb.stream) {
            StreamHead st{};
            memcpy(&st, sb.stream, sizeof(st));
            stype = st.streamType; sw = st.width; sh = st.height; sfmt = st.format;
        }

        // buffer.buffer points at ANativeWindowBuffer::handle, not at the struct.
        int32_t  aw = 0, ah = 0, astride = 0, afmt = 0;
        uint64_t ausage = 0;
        if (sb.buffer) {
            const void* anw = reinterpret_cast<const uint8_t*>(sb.buffer) - kAnwHandleOff;
            aw      = load<int32_t>(anw, kAnwWidthOff);
            ah      = load<int32_t>(anw, kAnwHeightOff);
            astride = load<int32_t>(anw, kAnwStrideOff);
            afmt    = load<int32_t>(anw, kAnwFormatOff);
            ausage  = load<uint64_t>(anw, kAnwUsageOff);
        }

        __atomic_add_fetch(&g_frames, 1, __ATOMIC_RELAXED);
        ShmHeader* diagnostics = shmsource_header();

        // ---- substitute -----------------------------------------------------
        const char* skip = nullptr;

        if (!g_shm && !g_bars)                  skip = "no source";
        else if (sb.status != 0)                skip = "buffer status error";
        else if (!sb.buffer)                    skip = "null buffer";
        else if (!framehook_format_supported(afmt))       skip = "unsupported format";

        if (!skip && !camera_geometry_supported(static_cast<uint32_t>(aw), static_cast<uint32_t>(ah)))
            skip = "unsupported dimensions";

        if (!skip && afmt == kQcomNv12Ubwc) {
            FrameSource* used = qcom_ubwc_fill(*sb.buffer, aw, ah, sb.releaseFence, g_shm, g_bars);
            if (used) {
                const uint64_t count = __atomic_add_fetch(&g_substituted, 1, __ATOMIC_RELAXED);
                if (used == g_bars && diagnostics)
                    __atomic_add_fetch(&diagnostics->fallbackFrames, 1, __ATOMIC_RELAXED);
                if (log_stream(thiz, used->name(), aw, ah, afmt))
                    LOGI("sub %llu [%s] stream=%p %dx%d QCOM_NV12_UBWC via C2D",
                         static_cast<unsigned long long>(count), used->name(), thiz, aw, ah);
            } else {
                skip = "UBWC conversion unavailable";
            }
        }

        if (!skip && afmt != kQcomNv12Ubwc) {
            void* anw = const_cast<uint8_t*>(
                reinterpret_cast<const uint8_t*>(sb.buffer)) - kAnwHandleOff;

            LockedBuffer lb;
            // The fence is duplicated, not consumed; the camera keeps its own.
            bool locked = (afmt != 0x22) &&
                          bufferlock_acquire(anw, sb.releaseFence, &lb);

            // lockPlanes cannot describe IMPLEMENTATION_DEFINED; the driver can.
            const bool usedMapper = !locked;
            if (!locked)
                locked = bufferlock_acquire_handle(*sb.buffer, aw, ah,
                                                   sb.releaseFence, &lb);

            if (!locked) {
                skip = "lock failed";
            } else {
                FrameTarget t;
                t.width  = aw;
                t.height = ah;
                t.format = afmt;
                t.y  = { lb.plane[0].data, lb.plane[0].rowStride, lb.plane[0].pixelStride };
                t.cb = { lb.plane[1].data, lb.plane[1].rowStride, lb.plane[1].pixelStride };
                t.cr = { lb.plane[2].data, lb.plane[2].rowStride, lb.plane[2].pixelStride };

                // Bars are the fallback, so an un-fed hook is still visibly live.
                FrameSource* used = nullptr;
                bool filled = false;
                if (!t.valid()) {
                    skip = "invalid plane layout";
                    if (log_stream(thiz, skip, aw, ah, afmt, &t))
                        LOGW("layout rejected stream=%p %dx%d fmt=0x%x via %s: "
                             "Y rs=%d ps=%d Cb rs=%d ps=%d Cr rs=%d ps=%d",
                             thiz, aw, ah, afmt, usedMapper ? "mapper" : "lockPlanes",
                             t.y.rowStride, t.y.pixelStride, t.cb.rowStride, t.cb.pixelStride,
                             t.cr.rowStride, t.cr.pixelStride);
                }
                else if (g_shm && g_shm->fill(t))  { filled = true; used = g_shm;  }
                else if (g_bars && g_bars->fill(t)) { filled = true; used = g_bars; }

                bufferlock_release(&lb);

                if (filled) {
                    const uint64_t count = __atomic_add_fetch(&g_substituted, 1, __ATOMIC_RELAXED);
                    if (used == g_bars && diagnostics)
                        __atomic_add_fetch(&diagnostics->fallbackFrames, 1, __ATOMIC_RELAXED);
                    if (log_stream(thiz, used->name(), aw, ah, afmt, &t)) {
                        LOGI("sub %llu [%s] stream=%p %dx%d %s via %s | Y rs=%d ps=%d | "
                             "Cb rs=%d ps=%d | Cr rs=%d ps=%d | Cb-Cr=%lld | Cb-Y=%lld Cr-Y=%lld",
                             static_cast<unsigned long long>(count),
                             used->name(), thiz, aw, ah, fmt_name(afmt),
                             usedMapper ? "mapper" : "lockPlanes",
                             t.y.rowStride, t.y.pixelStride,
                             t.cb.rowStride, t.cb.pixelStride,
                             t.cr.rowStride, t.cr.pixelStride,
                             static_cast<long long>(reinterpret_cast<intptr_t>(t.cb.data) -
                                                    reinterpret_cast<intptr_t>(t.cr.data)),
                             static_cast<long long>(reinterpret_cast<intptr_t>(t.cb.data) -
                                                    reinterpret_cast<intptr_t>(t.y.data)),
                             static_cast<long long>(reinterpret_cast<intptr_t>(t.cr.data) -
                                                    reinterpret_cast<intptr_t>(t.y.data)));
                        qcom::Handle handle;
                        if (qcom::read_handle(*sb.buffer, handle))
                            LOGI("QCOM capture layout: stream=%p format=0x%x flags=0x%x "
                                 "aligned=%dx%d requested=%dx%d size=%u",
                                 thiz, handle.format, handle.flags, handle.width, handle.height,
                                 handle.requestedWidth, handle.requestedHeight, handle.size);
                    }
                } else if (!skip) {
                    skip = "source declined";
                }
            }
        }

        if (skip) {
            const uint64_t count = __atomic_add_fetch(&g_passedThrough, 1, __ATOMIC_RELAXED);
            if (diagnostics) {
                if (strcmp(skip, "unsupported format") == 0 ||
                    strcmp(skip, "UBWC conversion unavailable") == 0) {
                    __atomic_add_fetch(&diagnostics->unsupportedFormat, 1, __ATOMIC_RELAXED);
                    __atomic_store_n(&diagnostics->lastUnsupportedFormat, static_cast<uint32_t>(afmt), __ATOMIC_RELAXED);
                } else if (strcmp(skip, "lock failed") == 0)
                    __atomic_add_fetch(&diagnostics->lockFailed, 1, __ATOMIC_RELAXED);
                else if (strcmp(skip, "invalid plane layout") == 0)
                    __atomic_add_fetch(&diagnostics->invalidLayout, 1, __ATOMIC_RELAXED);
                else if (strcmp(skip, "unsupported dimensions") == 0)
                    __atomic_add_fetch(&diagnostics->unsupportedDimensions, 1, __ATOMIC_RELAXED);
            }
            if (log_stream(thiz, skip, aw, ah, afmt))
                LOGI("pass %llu (%s) stream=%p type=%d %ux%u fmt=0x%x(%s) | buf %dx%d "
                     "fmt=0x%x(%s) stride=%d usage=0x%llx | status=%d rfence=%d",
                     static_cast<unsigned long long>(count), skip, thiz,
                     stype, sw, sh, sfmt, fmt_name(sfmt), aw, ah, afmt, fmt_name(afmt), astride,
                     static_cast<unsigned long long>(ausage),
                     sb.status, sb.releaseFence);
        }
    }

    // Forward all eight argument registers: the tail of the list is ABI-variant specific.
    if (!g_orig) return 0;
    return g_orig(thiz, buffer, a2, a3, a4, a5, a6, a7);
}

}  // namespace

// No waiting on camera threads: the injector may have borrowed one of them.
// A late disabled callback only uses the immutable original trampoline.
bool framehook_prepare(uintptr_t target)
{
    if (g_hook.target && g_hook.target != target) return false;
    if (__atomic_load_n(&g_callbacks, __ATOMIC_SEQ_CST) != 0) return false;
    uint32_t state = __atomic_load_n(&g_lifecycle, __ATOMIC_SEQ_CST);
    if (state != Dormant && !(state == Running && !g_orig)) return false;
    return __atomic_compare_exchange_n(&g_lifecycle, &state, Preparing, false,
                                       __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}

bool framehook_install(uintptr_t target)
{
    if (g_hook.installed) {
        LOGW("framehook: already installed");
        return true;
    }
    if (!target) {
        LOGE("framehook: injector supplied no target address");
        return false;
    }

    HookResult r = hook_install(target, reinterpret_cast<void*>(&recam_on_return_buffer),
                                &g_hook);
    if (r != HookResult::Ok) {
        LOGE("framehook: install failed - %s", hook_result_str(r));
        return false;
    }

    if (!g_orig) g_orig = reinterpret_cast<TargetFn>(g_hook.trampoline);
    g_frames = 0;
    g_substituted = 0;
    g_passedThrough = 0;
    memset(g_streamLogs, 0, sizeof(g_streamLogs));

    // Without the lock API the hook still installs and logs, it just cannot substitute.
    if (bufferlock_init()) {
        g_shm  = shmsource_get();
        g_bars = colorbars_source();
    }
    qcom_ubwc_retry_initialization();
    __atomic_store_n(&g_lifecycle, Running, __ATOMIC_SEQ_CST);
    __atomic_store_n(&g_enabled, 1, __ATOMIC_SEQ_CST);
    LOGI("framehook: sources = %s + %s",
         g_shm ? "shm" : "(no shm)", g_bars ? "colorbars" : "(none)");
    LOGI("framehook: hooked returnBufferCheckedLocked at %p; "
         "stream diagnostics enabled",
         reinterpret_cast<void*>(target));
    return true;
}

bool framehook_remove()
{
    // Failed initialization never exposed a callback/trampoline. Reclaim its
    // ring synchronously and leave this image eligible for an initialization retry.
    if (!g_hook.installed && !g_orig) {
        shmsource_destroy();
        __atomic_store_n(&g_lifecycle, Dormant, __ATOMIC_SEQ_CST);
        return true;
    }
    if (g_hook.installed) {
        HookResult r = hook_remove(&g_hook);
        if (r != HookResult::Ok) {
            LOGE("framehook: remove failed - %s", hook_result_str(r));
            return false;
        }
    }
    __atomic_store_n(&g_enabled, 0, __ATOMIC_SEQ_CST);
    // Repeated stop must not retire (and destroy) an already dormant ring again.
    uint32_t state = __atomic_load_n(&g_lifecycle, __ATOMIC_SEQ_CST);
    if (state == Running || state == Preparing)
        __atomic_store_n(&g_lifecycle, Retiring, __ATOMIC_SEQ_CST);
    // Never wait here: the injector can borrow a thread already in our callback.
    // Its eventual scope exit reclaims the ring if other callbacks still use it.
    reclaim_ring_if_idle();
    // Sources are static and the image stays mapped. Keep these pointers valid
    // for callbacks already inside fill() when shutdown removes the entry hook.
    LOGI("framehook: removed after %llu frames (%llu substituted, %llu passed through)",
         static_cast<unsigned long long>(__atomic_load_n(&g_frames, __ATOMIC_RELAXED)),
         static_cast<unsigned long long>(__atomic_load_n(&g_substituted, __ATOMIC_RELAXED)),
         static_cast<unsigned long long>(__atomic_load_n(&g_passedThrough, __ATOMIC_RELAXED)));

    // g_orig stays valid: a thread may still be inside the handler.
    return true;
}

}  // namespace recam
