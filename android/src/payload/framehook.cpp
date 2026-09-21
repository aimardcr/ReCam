// The hook on Camera3OutputStream::returnBufferCheckedLocked.

#include <android/log.h>
#include <stdint.h>
#include <string.h>

#include "framehook.hpp"
#include "inlinehook.hpp"
#include "bufferlock.hpp"
#include "framesource.hpp"
#include "shmsource.hpp"

#define LOG_TAG "ReCam"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace recam {

// IMPLEMENTATION_DEFINED is accepted only because the gralloc driver can describe it.
bool framehook_format_supported(int32_t f)
{
    if (f == 0x23 || f == 0x32315659)        // YCbCr_420_888, YV12
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
const char*  g_lastSourceName = nullptr;



bool should_log(uint64_t n)
{
    return n <= 4 || (n % 300) == 0;
}

// Reasons are string literals, so pointer identity distinguishes them.
const char* g_seenReasons[8] = {};

// Each distinct skip reason is logged once.
bool first_time_reason(const char* reason)
{
    for (auto& seen : g_seenReasons) {
        if (seen == reason) return false;
        if (!seen) { seen = reason; return true; }
    }
    return false;
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
        default:         return "?";
    }
}

// --- the hook --------------------------------------------------------------------

extern "C" int64_t recam_on_return_buffer(void* thiz, const void* buffer,
                                          int64_t a2, int64_t a3, int64_t a4,
                                          int64_t a5, int64_t a6, int64_t a7)
{
    // HAL result callback thread: a fault here takes cameraserver down.
    if (buffer) {
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

        g_frames++;

        // ---- substitute -----------------------------------------------------
        const char* skip = nullptr;

        if (!g_shm && !g_bars)                  skip = "no source";
        else if (sb.status != 0)                skip = "buffer status error";
        else if (!sb.buffer)                    skip = "null buffer";
        else if (!framehook_format_supported(afmt))       skip = "unsupported format";

        if (!skip) {
            void* anw = const_cast<uint8_t*>(
                reinterpret_cast<const uint8_t*>(sb.buffer)) - kAnwHandleOff;

            LockedBuffer lb;
            // The fence is duplicated, not consumed; the camera keeps its own.
            bool locked = (afmt != 0x22) &&
                          bufferlock_acquire(anw, sb.releaseFence, &lb);

            // lockPlanes cannot describe IMPLEMENTATION_DEFINED; the driver can.
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
                if (g_shm && g_shm->fill(t))  { filled = true; used = g_shm;  }
                else if (g_bars && g_bars->fill(t)) { filled = true; used = g_bars; }

                bufferlock_release(&lb);

                if (filled && used->name() != g_lastSourceName) {
                    LOGI("source: now using '%s'", used->name());
                    g_lastSourceName = used->name();
                }

                if (filled) {
                    g_substituted++;
                    if (should_log(g_substituted))
                        LOGI("sub %llu [%s] stream=%p %dx%d %s | Y rs=%d ps=%d | "
                             "Cb rs=%d ps=%d | Cr rs=%d ps=%d",
                             static_cast<unsigned long long>(g_substituted),
                             used->name(), thiz, aw, ah, fmt_name(afmt),
                             t.y.rowStride, t.y.pixelStride,
                             t.cb.rowStride, t.cb.pixelStride,
                             t.cr.rowStride, t.cr.pixelStride);
                } else {
                    skip = "source declined";
                }
            }
        }

        if (skip) {
            g_passedThrough++;
            if (first_time_reason(skip) || should_log(g_passedThrough))
                LOGI("pass %llu (%s) stream=%p type=%d %ux%u fmt=0x%x(%s) | buf %dx%d "
                     "stride=%d usage=0x%llx | status=%d rfence=%d",
                     static_cast<unsigned long long>(g_passedThrough), skip, thiz,
                     stype, sw, sh, sfmt, fmt_name(sfmt), aw, ah, astride,
                     static_cast<unsigned long long>(ausage),
                     sb.status, sb.releaseFence);
        }
    }

    // Forward all eight argument registers: the tail of the list is ABI-variant specific.
    if (!g_orig) return 0;
    return g_orig(thiz, buffer, a2, a3, a4, a5, a6, a7);
}

}  // namespace

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

    g_orig = reinterpret_cast<TargetFn>(g_hook.trampoline);
    g_frames = 0;
    g_substituted = 0;
    g_passedThrough = 0;
    memset(g_seenReasons, 0, sizeof(g_seenReasons));

    // Without the lock API the hook still installs and logs, it just cannot substitute.
    if (bufferlock_init()) {
        g_shm  = shmsource_get();
        g_bars = colorbars_source();
    } else {
        g_shm = g_bars = nullptr;
    }
    g_lastSourceName = nullptr;
    LOGI("framehook: sources = %s + %s",
         g_shm ? "shm" : "(no shm)", g_bars ? "colorbars" : "(none)");
    LOGI("framehook: hooked returnBufferCheckedLocked at %p; "
         "expect one line per frame from here",
         reinterpret_cast<void*>(target));
    return true;
}

bool framehook_remove()
{
    if (!g_hook.installed) return true;

    HookResult r = hook_remove(&g_hook);
    if (r != HookResult::Ok) {
        LOGE("framehook: remove failed - %s", hook_result_str(r));
        return false;
    }
    g_shm = g_bars = nullptr;
    LOGI("framehook: removed after %llu frames (%llu substituted, %llu passed through)",
         static_cast<unsigned long long>(g_frames),
         static_cast<unsigned long long>(g_substituted),
         static_cast<unsigned long long>(g_passedThrough));

    // g_orig stays valid: a thread may still be inside the handler.
    return true;
}

}  // namespace recam
