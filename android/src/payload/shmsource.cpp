// FrameSource backed by the shared frame ring.

#include "shmsource.hpp"
#include "blit.hpp"

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
        if (!g_base || !t.valid()) return false;
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
        // Keeps the lane alive; the feeder retires any lane nobody has asked about.
        __atomic_store_n(&l->reqNs, now_ns(), __ATOMIC_RELEASE);

        RingFrame frame(*l);
        if (!frame.seq) return false;
        const uint64_t stamp = l->slotNs[frame.slot];
        const uint64_t now = now_ns();
        if (now > stamp && now - stamp > kFrameStaleNs) return false;

        if (!lane_configured(*l)) return false;

        const uint64_t off = static_cast<uint64_t>(l->dataOffset) +
                             static_cast<uint64_t>(frame.slot) * l->bufBytes;
        if (off + l->bufBytes > g_size) return false;

        if (!blit_i420(g_base + off, l->width, l->height, t)) return false;
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

}  // namespace recam
