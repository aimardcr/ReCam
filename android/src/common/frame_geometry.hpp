#pragma once
#include <stdint.h>

namespace recam {
// Bound work on camera callback threads independently of the ring allocation.
constexpr uint32_t kCameraMaxDimension = 8192;
constexpr uint64_t kCameraMaxPixels = 32ull * 1024 * 1024;

inline bool camera_geometry_supported(uint32_t w, uint32_t h) {
    return w >= 2 && h >= 2 && w <= kCameraMaxDimension && h <= kCameraMaxDimension &&
           !((w | h) & 1) && static_cast<uint64_t>(w) * h <= kCameraMaxPixels;
}

// Fit to an even I420 size without enlarging the ring or changing camera buffers.
inline bool fit_frame(uint32_t w, uint32_t h, uint32_t maxW, uint32_t maxH,
                      uint32_t& outW, uint32_t& outH) {
    outW = outH = 0;
    if (!camera_geometry_supported(w, h) || maxW < 2 || maxH < 2) return false;
    if (w <= maxW && h <= maxH) { outW = w; outH = h; return true; }
    if (static_cast<uint64_t>(w) * maxH > static_cast<uint64_t>(h) * maxW) {
        outW = maxW & ~1u;
        outH = static_cast<uint32_t>(static_cast<uint64_t>(h) * outW / w) & ~1u;
    } else {
        outH = maxH & ~1u;
        outW = static_cast<uint32_t>(static_cast<uint64_t>(w) * outH / h) & ~1u;
    }
    return outW >= 2 && outH >= 2;
}
} // namespace recam
