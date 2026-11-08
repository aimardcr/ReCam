// FrameSource backed by the shared frame ring.

#include "shmsource.hpp"

#include <android/log.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include "../common/shmframe.hpp"
#include "../common/ring_access.hpp"

#define LOG_TAG "ReCam"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace recam {

namespace {

// Past this, treat the feeder as gone and fall back.
static const uint64_t kStaleNs = 2ull * 1000 * 1000 * 1000;

int      g_fd   = -1;
uint8_t* g_base = nullptr;
uint64_t g_size = 0;

ShmHeader* header() { return reinterpret_cast<ShmHeader*>(g_base); }

uint64_t now_ns()
{
    struct timespec ts {};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull + static_cast<uint64_t>(ts.tv_nsec);
}

int memfd_create_compat(const char* name, unsigned flags)
{
#ifdef SYS_memfd_create
    return static_cast<int>(syscall(SYS_memfd_create, name, flags));
#else
    (void)name; (void)flags;
    return -1;
#endif
}

// Every offset comes from the target's own rowStride and pixelStride.
bool blit_i420(const uint8_t* src, uint32_t w, uint32_t h, const FrameTarget& t)
{
    if (static_cast<uint32_t>(t.width) != w || static_cast<uint32_t>(t.height) != h)
        return false;   // geometry mismatch: let the real frame through

    const uint8_t* sy = src;
    const uint8_t* su = sy + static_cast<size_t>(w) * h;
    const uint8_t* sv = su + static_cast<size_t>(w / 2) * (h / 2);

    // Luma.
    for (uint32_t row = 0; row < h; row++) {
        uint8_t* d = t.y.data + static_cast<size_t>(row) * t.y.rowStride;
        const uint8_t* s = sy + static_cast<size_t>(row) * w;
        if (t.y.pixelStride == 1) {
            memcpy(d, s, w);
        } else {
            for (uint32_t x = 0; x < w; x++) d[x * t.y.pixelStride] = s[x];
        }
    }

    // pixelStride 2 means Cb and Cr interleave, so neither may be memcpy'd.
    const uint32_t cw = w / 2, ch = h / 2;
    for (uint32_t row = 0; row < ch; row++) {
        uint8_t* du = t.cb.data + static_cast<size_t>(row) * t.cb.rowStride;
        uint8_t* dv = t.cr.data + static_cast<size_t>(row) * t.cr.rowStride;
        const uint8_t* s_u = su + static_cast<size_t>(row) * cw;
        const uint8_t* s_v = sv + static_cast<size_t>(row) * cw;

        if (t.cb.pixelStride == 1) memcpy(du, s_u, cw);
        else for (uint32_t x = 0; x < cw; x++) du[x * t.cb.pixelStride] = s_u[x];

        if (t.cr.pixelStride == 1) memcpy(dv, s_v, cw);
        else for (uint32_t x = 0; x < cw; x++) dv[x * t.cr.pixelStride] = s_v[x];
    }
    return true;
}

// Claim a free lane for w x h. Two HAL threads can race; the loser retries next frame.
ShmLane* claim_lane(ShmHeader* h, uint32_t w, uint32_t h_)
{
    for (uint32_t i = 0; i < kShmLanes; i++) {
        ShmLane& l = h->lane[i];
        RingWrite access(&l.access);
        if (!access || l.reqWidth) continue;
        __atomic_store_n(&l.reqWidth, w, __ATOMIC_RELEASE);
        l.reqHeight = h_;
        __atomic_store_n(&l.reqNs, now_ns(), __ATOMIC_RELEASE);
        LOGI("shm: lane %u now asking for %ux%u", i, w, h_);
        return &l;
    }
    return nullptr;
}

class ShmSource final : public FrameSource {
public:
    const char* name() const override { return "shm"; }

    bool fill(const FrameTarget& t) override
    {
        if (!g_base) return false;
        ShmHeader* h = header();

        const uint32_t w = static_cast<uint32_t>(t.width);
        const uint32_t hh = static_cast<uint32_t>(t.height);

        for (uint32_t i = 0; i < kShmLanes; ++i) {
            ShmLane& lane = h->lane[i];
            RingRead access(&lane.access);
            if (access && lane.reqWidth == w && lane.reqHeight == hh)
                return fill_lane(t, h, &lane);
        }
        claim_lane(h, w, hh);
        return false; // a new geometry must be served before it can be copied
    }

private:
    bool fill_lane(const FrameTarget& t, ShmHeader* h, ShmLane* l)
    {
        const uint32_t w = static_cast<uint32_t>(t.width);
        const uint32_t hh = static_cast<uint32_t>(t.height);
        // Keeps the lane alive; the feeder retires any lane nobody has asked about.
        __atomic_store_n(&l->reqNs, now_ns(), __ATOMIC_RELEASE);

        RingFrame frame(*l);
        if (!frame.seq) return false;
        const uint64_t seq = frame.seq;
        const uint64_t stamp = l->slotNs[frame.slot];
        const uint64_t now = now_ns();
        if (now > stamp && now - stamp > kStaleNs) return false;

        if (l->width != w || l->height != hh || l->bufBytes == 0) return false;

        const uint64_t off = static_cast<uint64_t>(l->dataOffset) +
                             static_cast<uint64_t>(frame.slot) * l->bufBytes;
        if (off + l->bufBytes > g_size) return false;

        const bool profiling = __atomic_load_n(&h->profiling, __ATOMIC_ACQUIRE) != 0;
        const uint64_t copyStart = profiling ? now_ns() : 0;
        if (!blit_i420(g_base + off, w, hh, t)) return false;
        const uint64_t copyEnd = profiling ? now_ns() : 0;

        if (profiling) {
            __android_log_print(ANDROID_LOG_INFO, "ReCamLatency",
                "event=consume lane=%u seq=%llu start=%llu end=%llu",
                static_cast<unsigned>(l - h->lane), (unsigned long long)seq,
                (unsigned long long)copyStart, (unsigned long long)copyEnd);
        }
        __atomic_add_fetch(&h->consumed, 1, __ATOMIC_RELAXED);
        return true;
    }

};

ShmSource g_source;

}  // namespace

bool shmsource_create(int* fdOut, uint64_t* sizeOut)
{
    if (g_base) { *fdOut = g_fd; *sizeOut = g_size; return true; }

    const uint64_t size = shm_total_bytes();

    g_fd = memfd_create_compat("recam_frames", 0);
    if (g_fd < 0) {
        LOGE("shm: memfd_create failed");
        return false;
    }
    if (ftruncate(g_fd, static_cast<off_t>(size)) != 0) {
        LOGE("shm: ftruncate(%llu) failed", static_cast<unsigned long long>(size));
        close(g_fd); g_fd = -1;
        return false;
    }

    void* p = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, g_fd, 0);
    if (p == MAP_FAILED) {
        LOGE("shm: mmap failed");
        close(g_fd); g_fd = -1;
        return false;
    }

    g_base = static_cast<uint8_t*>(p);
    g_size = size;
    memset(g_base, 0, kShmArenaBase);

    ShmHeader* h = header();
    h->magic     = kShmMagic;
    h->version   = kShmVersion;
    h->laneCount = kShmLanes;

    // Assigned once here; the feeder never moves a lane, so a reader cannot be torn.
    for (uint32_t i = 0; i < kShmLanes; i++)
        h->lane[i].dataOffset = kShmArenaBase + i * lane_slice_bytes();

    LOGI("shm: ring ready - fd %d, %llu bytes, %u lanes x %u buffers of up to %ux%u",
         g_fd, static_cast<unsigned long long>(size), kShmLanes, kLaneBufs,
         kShmMaxWidth, kShmMaxHeight);

    *fdOut = g_fd;
    *sizeOut = size;
    return true;
}

void shmsource_destroy()
{
    if (g_base) {
        munmap(g_base, g_size);
        g_base = nullptr;
        g_size = 0;
    }
    if (g_fd >= 0) { close(g_fd); g_fd = -1; }
    LOGI("shm: retired frame ring released");
}

FrameSource* shmsource_get() { return g_base ? &g_source : nullptr; }

ShmHeader* shmsource_header() { return g_base ? header() : nullptr; }
uint8_t*   shmsource_base()   { return g_base; }

}  // namespace recam
