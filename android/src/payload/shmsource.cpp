// FrameSource backed by the shared frame ring.

#include "shmsource.hpp"

#include <android/log.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include "../common/shmframe.hpp"

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

// The lane already serving w x h, or nullptr. Runs on every HAL callback, so linear.
ShmLane* find_lane(ShmHeader* h, uint32_t w, uint32_t h_)
{
    for (uint32_t i = 0; i < kShmLanes; i++) {
        ShmLane& l = h->lane[i];
        if (l.reqWidth == w && l.reqHeight == h_) return &l;
    }
    return nullptr;
}

// Claim a free lane for w x h. Two HAL threads can race; the loser retries next frame.
ShmLane* claim_lane(ShmHeader* h, uint32_t w, uint32_t h_)
{
    for (uint32_t i = 0; i < kShmLanes; i++) {
        ShmLane& l = h->lane[i];
        uint32_t expected = 0;
        if (!__atomic_compare_exchange_n(&l.reqWidth, &expected, w, false,
                                         __ATOMIC_ACQ_REL, __ATOMIC_RELAXED))
            continue;
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

        ShmLane* l = find_lane(h, w, hh);
        if (!l) {
            if (!claim_lane(h, w, hh) && !m_warnedFull) {
                LOGW("shm: all %u lanes are busy; %ux%u falls back", kShmLanes, w, hh);
                m_warnedFull = true;
            }
            return false;   // the feeder has not carved this size yet
        }

        // Keeps the lane alive; the feeder retires any lane nobody has asked about.
        __atomic_store_n(&l->reqNs, now_ns(), __ATOMIC_RELEASE);

        const uint64_t seq = __atomic_load_n(&l->published, __ATOMIC_ACQUIRE);
        if (seq == 0) return false;

        if (now_ns() - l->publishedNs > kStaleNs) {
            if (!m_warnedStale) {
                LOGW("shm: no frame for >2s - feeder gone? falling back");
                m_warnedStale = true;
            }
            return false;
        }
        m_warnedStale = false;

        if (l->width != w || l->height != hh || l->bufBytes == 0) return false;

        const uint64_t off = static_cast<uint64_t>(l->dataOffset) +
                             static_cast<uint64_t>((seq - 1) % kLaneBufs) * l->bufBytes;
        if (off + l->bufBytes > g_size) return false;

        if (!blit_i420(g_base + off, w, hh, t)) return false;

        // Producer lapped us mid-copy: the frame we wrote is torn.
        if (__atomic_load_n(&l->published, __ATOMIC_ACQUIRE) - seq >= kLaneBufs) {
            h->dropped = h->dropped + 1;
            return false;
        }

        h->consumed = h->consumed + 1;
        return true;
    }

private:
    bool m_warnedStale = false;
    bool m_warnedFull  = false;
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

FrameSource* shmsource_get() { return g_base ? &g_source : nullptr; }

ShmHeader* shmsource_header() { return g_base ? header() : nullptr; }
uint8_t*   shmsource_base()   { return g_base; }

}  // namespace recam
