// The shared frame ring between the feeder and the payload.

#pragma once

#include <stdint.h>
#include "frame_geometry.hpp"

namespace recam {

// "RECAMSHM" little-endian.
static const uint64_t kShmMagic   = 0x4D48534D41434552ULL;
static const uint32_t kShmVersion = 6;

// Distinct camera buffer sizes served at once; a session rarely opens more.
static const uint32_t kShmLanes = 4;

// Readers pin slots; the producer never overwrites the current publication.
static const uint32_t kLaneBufs = 3;

// Page-aligned, so no lane buffer shares a page with the header.
static const uint32_t kShmArenaBase = 4096;

// Maximum stored frame. Larger camera outputs are scaled on consumption.
// The ring reserves 35.9 MiB regardless of requested camera size.
static const uint32_t kShmMaxWidth  = 1920;
static const uint32_t kShmMaxHeight = 1088;

// One lane per distinct size. The payload asks, the feeder carves and fills.
struct ShmLane {
    // Asked for by the payload. reqWidth 0 means the lane is free.
    volatile uint32_t reqWidth;
    volatile uint32_t reqHeight;
    volatile uint64_t reqNs;        // CLOCK_MONOTONIC; the feeder retires stale lanes

    // Answered by the feeder. Zero until the arena has been carved for this size.
    volatile uint32_t width;
    volatile uint32_t height;
    volatile uint32_t dataOffset;   // from the base of the mapping
    volatile uint32_t bufBytes;     // one buffer; the lane holds kLaneBufs of them
    volatile uint64_t published;    // the newest buffer is (published - 1) % kLaneBufs
    volatile uint64_t publishedNs;

    // Metadata is immutable while users hold shared access; exclusive access is
    // used only to claim/configure/retire a lane. Slot locks protect pixel copies.
    uint32_t access;
    uint32_t slots[kLaneBufs];
    uint64_t slotSeq[kLaneBufs];
    uint64_t slotNs[kLaneBufs];
};

struct ShmHeader {
    uint64_t magic;
    uint32_t version;
    uint32_t laneCount;

    // Activity counters summed across lanes.
    volatile uint64_t published;
    volatile uint64_t consumed;
    volatile uint64_t dropped;

    ShmLane lane[kShmLanes];

    uint64_t producerSkipped;  // all non-current slots pinned; last frame retained
};

static_assert(sizeof(ShmHeader) <= kShmArenaBase, "header exceeds reserved page");

inline bool lane_geometry(uint32_t w, uint32_t h, uint32_t& storedW, uint32_t& storedH) {
    return fit_frame(w, h, kShmMaxWidth, kShmMaxHeight, storedW, storedH);
}

inline bool lane_configured(const ShmLane& l) {
    uint32_t w, h;
    return lane_geometry(l.reqWidth, l.reqHeight, w, h) && l.width == w && l.height == h &&
           l.bufBytes == w * h * 3 / 2;
}

constexpr uint64_t kFrameStaleNs = 2000000000ull;
inline const char* lane_state(const ShmLane& l, bool feeder, uint64_t now) {
    uint32_t w, h;
    if (!lane_geometry(l.reqWidth, l.reqHeight, w, h)) return "unsupported_dimensions";
    if (!feeder) return "feeder_stopped";
    if (!lane_configured(l) || !l.published || !l.publishedNs) return "waiting_for_frame";
    if (now > l.publishedNs && now - l.publishedNs > kFrameStaleNs) return "stale_frame";
    return "served";
}

inline uint32_t lane_frame_bytes(uint32_t w, uint32_t h) { return w * h * 3 / 2; }

// Every lane gets the same slice, so an offset never moves under a reader.
inline uint32_t lane_slice_bytes()
{
    return kLaneBufs * lane_frame_bytes(kShmMaxWidth, kShmMaxHeight);
}

// Mostly untouched: a lane serving a small size dirties only its first few pages.
inline uint64_t shm_total_bytes()
{
    return kShmArenaBase + static_cast<uint64_t>(kShmLanes) * lane_slice_bytes();
}

}  // namespace recam
