// The shared frame ring between the feeder and the payload.

#pragma once

#include <stdint.h>

namespace recam {

// "RECAMSHM" little-endian.
static const uint64_t kShmMagic   = 0x4D48534D41434552ULL;
static const uint32_t kShmVersion = 2;

// Distinct camera buffer sizes served at once; a session rarely opens more.
static const uint32_t kShmLanes = 4;

// Enough that the producer cannot lap the consumer mid-copy.
static const uint32_t kLaneBufs = 3;

// Page-aligned, so no lane buffer shares a page with the header.
static const uint32_t kShmArenaBase = 4096;

// The largest frame a lane can hold. Bigger streams fall back to the real camera.
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
};

struct ShmHeader {
    uint64_t magic;
    uint32_t version;
    uint32_t laneCount;

    // Diagnostics only, summed across lanes.
    volatile uint64_t published;
    volatile uint64_t consumed;
    volatile uint64_t dropped;

    ShmLane lane[kShmLanes];
};

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
