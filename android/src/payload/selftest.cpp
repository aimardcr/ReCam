// Payload self-test - exercises the paths the real camera never reaches.

#include <android/hardware_buffer.h>
#include <android/log.h>
#include <dlfcn.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
#include <time.h>

#include "bufferlock.hpp"
#include "framesource.hpp"
#include "inlinehook.hpp"
#include "framehook.hpp"
#include "shmsource.hpp"

#define LOG_TAG "ReCamTest"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// Not declared in the NDK header.
#ifndef AHARDWAREBUFFER_FORMAT_YV12
#define AHARDWAREBUFFER_FORMAT_YV12 0x32315659u
#endif

extern "C" {
int recam_test_plain(int x);
int recam_test_bti(int x);
int recam_test_pac(int x);
int recam_test_adrp(int x);
int recam_test_ldrlit(int x);
}

namespace recam {
namespace {

int g_pass = 0;
int g_fail = 0;

void check(bool ok, const char* what, const char* detail = nullptr)
{
    if (ok) { g_pass++; LOGI("  PASS  %s", what); }
    else    { g_fail++; LOGE("  FAIL  %s%s%s", what, detail ? " - " : "", detail ? detail : ""); }
}

// --------------------------------------------------------------- hook tests

using TestFn = int (*)(int);

uintptr_t g_tramp = 0;
int       g_hits  = 0;

// +100 on the original result, so one value proves both the detour and the trampoline.
extern "C" int recam_test_handler(int x)
{
    g_hits++;
    return reinterpret_cast<TestFn>(g_tramp)(x) + 100;
}

// A prologue the hook must accept.
void test_hookable(const char* name, TestFn fn, uint32_t expectPatchOffset)
{
    LOGI("[hook] %s", name);

    const int before = fn(5);
    check(before == 6, "original returns x+1 before hooking");

    uint8_t orig[16];
    memcpy(orig, reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(fn) + expectPatchOffset), 16);

    InlineHook h;
    g_hits = 0;
    HookResult r = hook_install(reinterpret_cast<uintptr_t>(fn),
                                reinterpret_cast<void*>(&recam_test_handler), &h);
    if (r != HookResult::Ok) {
        check(false, "hook_install succeeded", hook_result_str(r));
        return;
    }
    g_tramp = h.trampoline;

    check(h.patchOffset == expectPatchOffset, "patch offset is as expected");

    // With BTI, the landing pad must still be the first instruction at the entry.
    if (expectPatchOffset == 4) {
        uint32_t first = *reinterpret_cast<uint32_t*>(fn);
        check((first & 0xFFFFFF1Fu) == 0xD503241Fu,
              "BTI landing pad survived the patch");
    }

    const int hooked = fn(5);
    check(g_hits == 1, "handler ran exactly once");
    check(hooked == 106, "trampoline still executed the original body");

    r = hook_remove(&h);
    check(r == HookResult::Ok, "hook_remove succeeded", hook_result_str(r));

    const bool restored = memcmp(
        orig, reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(fn) + expectPatchOffset), 16) == 0;
    check(restored, "original bytes restored exactly");

    g_hits = 0;
    check(fn(5) == 6 && g_hits == 0, "original behaviour back after unhook");
}

// A prologue the hook must refuse - and it must not have written anything first.
void test_refused(const char* name, TestFn fn)
{
    LOGI("[hook] %s (must be refused)", name);

    uint8_t orig[16];
    memcpy(orig, reinterpret_cast<void*>(fn), 16);

    InlineHook h;
    HookResult r = hook_install(reinterpret_cast<uintptr_t>(fn),
                                reinterpret_cast<void*>(&recam_test_handler), &h);

    check(r == HookResult::PrologueUnrelocatable,
          "refused with PrologueUnrelocatable", hook_result_str(r));
    check(!h.installed, "hook not marked installed");
    check(memcmp(orig, reinterpret_cast<void*>(fn), 16) == 0,
          "target text untouched after refusal");
    check(fn(5) == 6, "function still works after refusal");

    if (h.installed) hook_remove(&h);
}

// --------------------------------------------------------------- buffer tests

using PFN_alloc   = int  (*)(const AHardwareBuffer_Desc*, AHardwareBuffer**);
using PFN_release = void (*)(AHardwareBuffer*);
using PFN_lockP   = int  (*)(AHardwareBuffer*, uint64_t, int32_t, const ARect*,
                             AHardwareBuffer_Planes*);
using PFN_unlock  = int  (*)(AHardwareBuffer*, int32_t*);
using PFN_describe = void (*)(const AHardwareBuffer*, AHardwareBuffer_Desc*);
using PFN_lockInfo = int  (*)(AHardwareBuffer*, uint64_t, int32_t, const ARect*, void**, int32_t*, int32_t*);

PFN_alloc   p_alloc   = nullptr;
PFN_release p_release = nullptr;
PFN_lockP   p_lockP   = nullptr;
PFN_unlock   p_unlock   = nullptr;
PFN_describe p_describe = nullptr;
PFN_lockInfo p_lockInfo = nullptr;

bool resolve_alloc_api()
{
    p_alloc   = reinterpret_cast<PFN_alloc>(dlsym(RTLD_DEFAULT, "AHardwareBuffer_allocate"));
    p_release = reinterpret_cast<PFN_release>(dlsym(RTLD_DEFAULT, "AHardwareBuffer_release"));
    p_lockP   = reinterpret_cast<PFN_lockP>(dlsym(RTLD_DEFAULT, "AHardwareBuffer_lockPlanes"));
    p_unlock  = reinterpret_cast<PFN_unlock>(dlsym(RTLD_DEFAULT, "AHardwareBuffer_unlock"));
    p_describe = reinterpret_cast<PFN_describe>(dlsym(RTLD_DEFAULT, "AHardwareBuffer_describe"));
    p_lockInfo = reinterpret_cast<PFN_lockInfo>(dlsym(RTLD_DEFAULT, "AHardwareBuffer_lockAndGetInfo"));
    return p_alloc && p_release && p_lockP && p_unlock;
}

const char* fmt_name(uint32_t f)
{
    return f == AHARDWAREBUFFER_FORMAT_YV12 ? "YV12"
         : f == AHARDWAREBUFFER_FORMAT_Y8Cb8Cr8_420 ? "YCbCr_420_888" : "?";
}

// Round-trip through a real gralloc buffer: poison, write bars, verify.
void test_buffer(uint32_t format, int w, int h)
{
    LOGI("[buffer] %s %dx%d", fmt_name(format), w, h);

    AHardwareBuffer_Desc desc{};
    desc.width  = static_cast<uint32_t>(w);
    desc.height = static_cast<uint32_t>(h);
    desc.layers = 1;
    desc.format = format;
    desc.usage  = AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN |
                  AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN;

    AHardwareBuffer* buf = nullptr;
    if (p_alloc(&desc, &buf) != 0 || !buf) {
        check(false, "allocate test buffer", "gralloc refused this format/usage");
        return;
    }

    const uint8_t kPoison = 0xA5;

    // --- poison every byte, so anything we fail to write stays detectable ---
    AHardwareBuffer_Planes pl{};
    if (p_lockP(buf, AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN, -1, nullptr, &pl) != 0 ||
        pl.planeCount < 3) {
        check(false, "lock test buffer for poisoning");
        p_release(buf);
        return;
    }
    // Poison every row's full stride, including the padding past `width`.
    for (int p = 0; p < 3; p++) {
        if (p == 2 && pl.planes[2].data == static_cast<uint8_t*>(pl.planes[1].data) + 1)
            continue;   // already covered by plane 1
        const int rows = (p == 0) ? h : h / 2;
        memset(pl.planes[p].data, kPoison,
               static_cast<size_t>(pl.planes[p].rowStride) * static_cast<size_t>(rows));
    }
    int32_t f = -1;
    p_unlock(buf, &f);
    if (f >= 0) close(f);

    // --- write colour bars through the real path ---
    LockedBuffer lb;
    // _ahb: this is already an AHardwareBuffer, so the plain call would downcast twice.
    if (!bufferlock_acquire_ahb(reinterpret_cast<void*>(buf), -1, &lb)) {
        check(false, "bufferlock_acquire_ahb on a self-allocated buffer");
        p_release(buf);
        return;
    }

    FrameTarget t;
    t.width = w; t.height = h; t.format = static_cast<int32_t>(format);
    t.y  = { lb.plane[0].data, lb.plane[0].rowStride, lb.plane[0].pixelStride };
    t.cb = { lb.plane[1].data, lb.plane[1].rowStride, lb.plane[1].pixelStride };
    t.cr = { lb.plane[2].data, lb.plane[2].rowStride, lb.plane[2].pixelStride };

    LOGI("  layout: Y rs=%d ps=%d | Cb rs=%d ps=%d | Cr rs=%d ps=%d",
         t.y.rowStride, t.y.pixelStride, t.cb.rowStride, t.cb.pixelStride,
         t.cr.rowStride, t.cr.pixelStride);

    const bool filled = colorbars_source()->fill(t);
    bufferlock_release(&lb);
    check(filled, "colorbars fill() reported success");
    if (!filled) { p_release(buf); return; }

    // --- read back and verify ---
    if (p_lockP(buf, AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN, -1, nullptr, &pl) != 0) {
        check(false, "lock test buffer for readback");
        p_release(buf);
        return;
    }

    const uint8_t* y  = static_cast<const uint8_t*>(pl.planes[0].data);
    const int      ys = pl.planes[0].rowStride;

    // Sampled mid-bar, clear of any rounding at the boundaries.
    static const uint8_t kY[8] = { 255, 226, 179, 150, 105, 76, 29, 0 };
    bool lumaOk = true;
    for (int row = 0; row < h; row += (h / 7) + 1) {
        for (int bar = 0; bar < 8; bar++) {
            const int x = (bar * w) / 8 + (w / 16);      // middle of the bar
            const uint8_t got = y[static_cast<size_t>(row) * ys + x];
            if (got != kY[bar]) {
                LOGE("  luma mismatch row=%d bar=%d x=%d: got %u want %u",
                     row, bar, x, got, kY[bar]);
                lumaOk = false;
            }
        }
    }
    check(lumaOk, "luma matches the expected bar values at the right columns");

    // Bytes between width and rowStride must still be poison.
    bool padOk = true;
    int  padChecked = 0;
    if (ys > w) {
        for (int row = 0; row < h; row++) {
            for (int x = w; x < ys; x++) {
                padChecked++;
                if (y[static_cast<size_t>(row) * ys + x] != kPoison) {
                    LOGE("  stride padding clobbered at row=%d x=%d", row, x);
                    padOk = false;
                    row = h;   // one report is enough
                    break;
                }
            }
        }
        check(padOk, "luma stride padding untouched");
        LOGI("  checked %d padding bytes (rowStride %d > width %d)", padChecked, ys, w);
    } else {
        LOGI("  no luma padding on this buffer (rowStride == width == %d)", w);
    }

    // Honours pixelStride, so a semi-planar layout is checked correctly.
    static const uint8_t kCb[8] = { 128, 0, 171, 44, 212, 85, 255, 128 };
    static const uint8_t kCr[8] = { 128, 149, 0, 21, 235, 255, 107, 128 };
    const uint8_t* cb = static_cast<const uint8_t*>(pl.planes[1].data);
    const uint8_t* cr = static_cast<const uint8_t*>(pl.planes[2].data);
    const int cbs = pl.planes[1].rowStride, cbp = pl.planes[1].pixelStride;
    const int crs = pl.planes[2].rowStride, crp = pl.planes[2].pixelStride;

    bool chromaOk = true;
    for (int bar = 0; bar < 8; bar++) {
        const int cx = ((bar * w) / 8 + (w / 16)) / 2;   // chroma column for that bar
        const uint8_t gotCb = cb[static_cast<size_t>(cbs) * 2 + cx * cbp];
        const uint8_t gotCr = cr[static_cast<size_t>(crs) * 2 + cx * crp];
        if (gotCb != kCb[bar] || gotCr != kCr[bar]) {
            LOGE("  chroma mismatch bar=%d: cb %u/%u cr %u/%u",
                 bar, gotCb, kCb[bar], gotCr, kCr[bar]);
            chromaOk = false;
        }
    }
    check(chromaOk, "chroma matches, honouring pixelStride");

    f = -1;
    p_unlock(buf, &f);
    if (f >= 0) close(f);
    p_release(buf);
}

// --------------------------------------------------------------- fence ownership

// bufferlock_acquire must duplicate the fence it is given, never consume it.
void test_fence_ownership()
{
    LOGI("[fence] ownership");

    AHardwareBuffer_Desc desc{};
    desc.width = 64; desc.height = 64; desc.layers = 1;
    desc.format = AHARDWAREBUFFER_FORMAT_YV12;
    desc.usage = AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN |
                 AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN;

    AHardwareBuffer* buf = nullptr;
    if (p_alloc(&desc, &buf) != 0 || !buf) {
        check(false, "allocate buffer for fence test");
        return;
    }

    // Any fd will do: this tests ownership, not waiting.
    int probe = dup(STDIN_FILENO);
    if (probe < 0) probe = ::open("/dev/null", 0);
    if (probe < 0) {
        check(false, "could not make a probe fd");
        p_release(buf);
        return;
    }

    LockedBuffer lb;
    const bool locked = bufferlock_acquire_ahb(reinterpret_cast<void*>(buf), probe, &lb);
    if (locked) bufferlock_release(&lb);

    // The probe fd must still be open. fcntl(F_GETFD) fails with EBADF if it is not.
    const bool stillOpen = (fcntl(probe, F_GETFD) != -1);
    check(stillOpen, "caller's fence fd survived bufferlock_acquire (dup, not consume)");

    if (stillOpen) close(probe);
    p_release(buf);
}


// Can a buffer carrying the VIDEO ENCODER's usage flags be locked for CPU write?
void test_encoder_usage()
{
    LOGI("[encoder] CPU-write lock on a VIDEO_ENCODE buffer");

    AHardwareBuffer_Desc desc{};
    desc.width  = 1920;
    desc.height = 1080;
    desc.layers = 1;
    desc.format = AHARDWAREBUFFER_FORMAT_Y8Cb8Cr8_420;
    // GRALLOC_USAGE_HW_CAMERA_WRITE is 1<<17; the NDK header does not name it.
    desc.usage  = (1ULL << 17) |
                  AHARDWAREBUFFER_USAGE_VIDEO_ENCODE |
                  AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN;

    AHardwareBuffer* buf = nullptr;
    if (p_alloc(&desc, &buf) != 0 || !buf) {
        LOGI("  gralloc refused the allocation outright");
        return;
    }

    AHardwareBuffer_Planes pl{};
    const int rc = p_lockP(buf, AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN, -1, nullptr, &pl);
    if (rc == 0) {
        LOGI("  420_888 + VIDEO_ENCODE: lock OK, planes=%u", pl.planeCount);
        int32_t f = -1;
        p_unlock(buf, &f);
        if (f >= 0) close(f);
    } else {
        LOGI("  420_888 + VIDEO_ENCODE: lock REFUSED (rc=%d)", rc);
    }
    p_release(buf);

    // The real recording stream uses IMPLEMENTATION_DEFINED.
    desc.format = 0x22;
    buf = nullptr;
    if (p_alloc(&desc, &buf) != 0 || !buf) {
        LOGI("  IMPL_DEFINED + VIDEO_ENCODE: gralloc refused the allocation");
        return;
    }

    AHardwareBuffer_Desc got{};
    if (p_describe) { p_describe(buf, &got); LOGI("  IMPL_DEFINED describes back as format 0x%x, stride %u", got.format, got.stride); }

    AHardwareBuffer_Planes pl2{};
    const int rc2 = p_lockP(buf, AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN, -1, nullptr, &pl2);
    if (rc2 == 0) {
        LOGI("  IMPL_DEFINED: lockPlanes OK, planes=%u", pl2.planeCount);
        for (uint32_t i = 0; i < pl2.planeCount && i < 3; i++)
            LOGI("    plane %u: rs=%u ps=%u", i, pl2.planes[i].rowStride, pl2.planes[i].pixelStride);
        int32_t f = -1; p_unlock(buf, &f); if (f >= 0) close(f);
    } else {
        LOGI("  IMPL_DEFINED: lockPlanes REFUSED (rc=%d)", rc2);
    }

    if (p_lockInfo) {
        void* va = nullptr; int32_t bpp = 0, bps = 0;
        const int rc3 = p_lockInfo(buf, AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN, -1, nullptr, &va, &bpp, &bps);
        if (rc3 == 0) {
            LOGI("  IMPL_DEFINED: lockAndGetInfo OK, bytesPerPixel=%d bytesPerStride=%d", bpp, bps);
            int32_t f = -1; p_unlock(buf, &f); if (f >= 0) close(f);
        } else {
            LOGI("  IMPL_DEFINED: lockAndGetInfo REFUSED (rc=%d)", rc3);
        }
    }

    check(bufferlock_have_ycbcr(), "GraphicBufferMapper android_ycbcr path resolved");

    using PFN_getHandle = const void* (*)(const AHardwareBuffer*);
    auto p_handle = reinterpret_cast<PFN_getHandle>(
        dlsym(RTLD_DEFAULT, "AHardwareBuffer_getNativeHandle"));
    const void* nh = p_handle ? p_handle(buf) : nullptr;

    if (nh) {
        LockedBuffer lb;
        if (bufferlock_acquire_handle(nh, desc.width, desc.height, -1, &lb)) {
            LOGI("  IMPL_DEFINED: android_ycbcr OK | Y rs=%d ps=%d | Cb rs=%d ps=%d "
                 "| Cr rs=%d ps=%d | cr-cb=%td",
                 lb.plane[0].rowStride, lb.plane[0].pixelStride,
                 lb.plane[1].rowStride, lb.plane[1].pixelStride,
                 lb.plane[2].rowStride, lb.plane[2].pixelStride,
                 lb.plane[2].data - lb.plane[1].data);
            FrameTarget t;
            t.width = static_cast<int32_t>(desc.width);
            t.height = static_cast<int32_t>(desc.height);
            t.format = 0x22;
            t.y  = { lb.plane[0].data, lb.plane[0].rowStride, lb.plane[0].pixelStride };
            t.cb = { lb.plane[1].data, lb.plane[1].rowStride, lb.plane[1].pixelStride };
            t.cr = { lb.plane[2].data, lb.plane[2].rowStride, lb.plane[2].pixelStride };
            check(t.valid(), "android_ycbcr layout passes FrameTarget::valid()");
            check(colorbars_source()->fill(t), "colour bars fill an IMPL_DEFINED buffer");
            bufferlock_release(&lb);
            check(lb.nativeHandle == nullptr, "release cleared the mapper handle");
        } else {
            check(false, "android_ycbcr lock on an IMPL_DEFINED encoder buffer");
        }
    } else {
        check(false, "AHardwareBuffer_getNativeHandle resolved");
    }
    p_release(buf);
}

// --------------------------------------------------------------- pass-through logic

// A live camera reports status == 0 on every frame, so these branches need forcing.
void test_passthrough_rules()
{
    LOGI("[rules] format gating");
    check(framehook_format_supported(0x23),       "YCbCr_420_888 is substituted");
    check(framehook_format_supported(0x32315659), "YV12 is substituted");
    check(!framehook_format_supported(0x21),      "BLOB is NEVER substituted");
    check(!framehook_format_supported(0x20),      "RAW16 passes through");
    check(!framehook_format_supported(0x25),      "RAW10 passes through");
    check(framehook_format_supported(0x22) == bufferlock_have_ycbcr(),
          "IMPLEMENTATION_DEFINED is substituted only where the driver can describe it");
}

// --------------------------------------------------------------- shm lanes

uint64_t test_now_ns()
{
    struct timespec ts {};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull + static_cast<uint64_t>(ts.tv_nsec);
}

// A heap destination, so the lane test needs no gralloc.
struct HeapTarget {
    uint8_t*    mem = nullptr;
    FrameTarget t;

    void alloc(int32_t w, int32_t h)
    {
        const size_t bytes = static_cast<size_t>(w) * h * 3 / 2;
        mem = static_cast<uint8_t*>(malloc(bytes));
        memset(mem, 0, bytes);
        t.width = w; t.height = h; t.format = 0x23;
        t.y  = { mem,                                          w,     1 };
        t.cb = { mem + static_cast<size_t>(w) * h,             w / 2, 1 };
        t.cr = { mem + static_cast<size_t>(w) * h * 5 / 4,     w / 2, 1 };
    }
    void free_() { free(mem); mem = nullptr; }
};

// Plays the feeder: answer the lane's request and publish one flat frame.
void lane_publish(ShmLane* l, uint8_t luma)
{
    l->published = 0;
    l->bufBytes  = lane_frame_bytes(l->reqWidth, l->reqHeight);
    l->height    = l->reqHeight;
    l->width     = l->reqWidth;

    const size_t ySize = static_cast<size_t>(l->width) * l->height;
    uint8_t* buf = shmsource_base() + l->dataOffset;
    memset(buf, luma, ySize);
    memset(buf + ySize, 128, l->bufBytes - ySize);

    l->publishedNs = test_now_ns();
    l->published   = 1;
}

ShmLane* lane_for(ShmHeader* h, uint32_t w, uint32_t hh)
{
    for (uint32_t i = 0; i < kShmLanes; i++)
        if (h->lane[i].reqWidth == w && h->lane[i].reqHeight == hh) return &h->lane[i];
    return nullptr;
}

// Two streams of different sizes at once is the case the camera app cannot produce.
void test_shm_lanes()
{
    LOGI("[lanes] concurrent sizes");

    ShmHeader* h = shmsource_header();
    FrameSource* src = shmsource_get();
    if (!h || !src) { check(false, "the frame ring exists"); return; }

    for (uint32_t i = 0; i < kShmLanes; i++) {
        h->lane[i].reqWidth = 0; h->lane[i].reqHeight = 0;
        h->lane[i].width = 0; h->lane[i].height = 0;
        h->lane[i].bufBytes = 0; h->lane[i].published = 0;
    }

    HeapTarget a, b;
    a.alloc(1440, 1080);
    b.alloc(640, 480);

    check(!src->fill(a.t), "an unserved size declines rather than guessing");
    ShmLane* la = lane_for(h, 1440, 1080);
    check(la != nullptr, "asking for 1440x1080 claimed a lane");

    check(!src->fill(b.t), "a second unserved size also declines");
    ShmLane* lb = lane_for(h, 640, 480);
    check(lb != nullptr, "asking for 640x480 claimed a second lane");
    check(la != lb, "the two sizes hold different lanes");

    if (!la || !lb) { a.free_(); b.free_(); return; }

    lane_publish(la, 200);
    lane_publish(lb, 60);

    check(src->fill(a.t), "1440x1080 fills once its lane is served");
    check(a.mem[0] == 200 && a.mem[1440 * 1080 - 1] == 200, "1440x1080 got its own pixels");

    check(src->fill(b.t), "640x480 fills from the other lane, same instant");
    check(b.mem[0] == 60 && b.mem[640 * 480 - 1] == 60, "640x480 got its own pixels");

    check(a.mem[0] == 200, "serving the second lane did not disturb the first");

    // Buffers are a fixed slice apart, so neither lane can write into the other.
    check(lb->dataOffset - la->dataOffset == lane_slice_bytes(),
          "lanes are a full slice apart");

    HeapTarget over;
    over.alloc(320, 240);
    for (uint32_t i = 0; i < kShmLanes; i++) {
        if (h->lane[i].reqWidth) continue;
        h->lane[i].reqWidth = 800 + i; h->lane[i].reqHeight = 600;
    }
    check(!src->fill(over.t), "a size past the last free lane declines, and does not crash");
    over.free_();

    a.free_();
    b.free_();
    for (uint32_t i = 0; i < kShmLanes; i++) {
        h->lane[i].reqWidth = 0; h->lane[i].reqHeight = 0;
        h->lane[i].width = 0; h->lane[i].height = 0;
        h->lane[i].bufBytes = 0; h->lane[i].published = 0;
    }
}

}  // namespace

int selftest_run()
{
    g_pass = g_fail = 0;
    LOGI("================ ReCam self-test ================");

    test_hookable("plain prologue",        recam_test_plain, 0);
    test_hookable("BTI landing pad",       recam_test_bti,   4);
    test_hookable("PACIASP prologue",      recam_test_pac,   0);
    test_refused ("ADRP in prologue",      recam_test_adrp);
    test_refused ("LDR literal in prologue", recam_test_ldrlit);

    test_passthrough_rules();
    test_shm_lanes();

    if (!resolve_alloc_api() || !bufferlock_init()) {
        check(false, "resolve AHardwareBuffer API");
    } else {
        test_buffer(AHARDWAREBUFFER_FORMAT_YV12,          1440, 1080);
        test_buffer(AHARDWAREBUFFER_FORMAT_Y8Cb8Cr8_420,  1440, 1080);
        // A width that is not a multiple of common alignments, to force padding.
        test_buffer(AHARDWAREBUFFER_FORMAT_Y8Cb8Cr8_420,   642,  482);
        test_fence_ownership();
        test_encoder_usage();
    }

    LOGI("================ %d passed, %d failed ================", g_pass, g_fail);
    return g_fail;
}

}  // namespace recam
