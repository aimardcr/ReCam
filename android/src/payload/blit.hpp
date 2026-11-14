// Packed I420 to driver-described planes. No allocations or shared row cache.
#pragma once
#include "framesource.hpp"
#include <stddef.h>
#include <string.h>

namespace recam {
inline void write_plane_row(uint8_t* dst, const uint8_t* src, uint32_t count, int stride) {
    if (stride == 1) memcpy(dst, src, count);
    else for (uint32_t x = 0; x < count; ++x) dst[x * stride] = src[x];
}

inline void blit_plane(const uint8_t* src, uint32_t sw, uint32_t sh,
                       const FrameTarget::Plane& dst, uint32_t dw, uint32_t dh) {
    if (sw == dw && sh == dh) {
        for (uint32_t y = 0; y < dh; ++y)
            write_plane_row(dst.data + static_cast<size_t>(y) * dst.rowStride,
                            src + static_cast<size_t>(y) * sw, dw, dst.pixelStride);
        return;
    }
    // At most 16 KiB of scratch per callback, reused for all three planes.
    uint8_t rows[2][kCameraMaxDimension];
    int indices[2] = {-1, -1};
    unsigned top = 0;
    const uint32_t xr = dw > 1 ? ((sw - 1) << 16) / (dw - 1) : 0;
    const uint32_t yr = dh > 1 ? ((sh - 1) << 16) / (dh - 1) : 0;
    auto horizontal = [&](uint32_t y, unsigned slot) {
        if (indices[slot] == static_cast<int>(y)) return;
        const uint8_t* in = src + static_cast<size_t>(y) * sw;
        for (uint32_t x = 0, sx = 0; x < dw; ++x, sx += xr) {
            const uint32_t a = sx >> 16, b = a + 1 < sw ? a + 1 : a;
            rows[slot][x] = static_cast<uint8_t>(in[a] +
                ((static_cast<int>(in[b]) - in[a]) * static_cast<int>(sx & 65535) >> 16));
        }
        indices[slot] = static_cast<int>(y);
    };
    for (uint32_t y = 0, sy = 0; y < dh; ++y, sy += yr) {
        const uint32_t a = sy >> 16, b = a + 1 < sh ? a + 1 : a;
        const int fraction = sy & 65535;
        if (indices[top ^ 1] == static_cast<int>(a)) top ^= 1;
        horizontal(a, top);
        uint8_t* out = dst.data + static_cast<size_t>(y) * dst.rowStride;
        if (!fraction || a == b) {
            write_plane_row(out, rows[top], dw, dst.pixelStride);
            continue;
        }
        horizontal(b, top ^ 1);
        for (uint32_t x = 0; x < dw; ++x)
            out[x * dst.pixelStride] = static_cast<uint8_t>(rows[top][x] +
                ((static_cast<int>(rows[top ^ 1][x]) - rows[top][x]) * fraction >> 16));
    }
}

inline bool blit_i420(const uint8_t* src, uint32_t w, uint32_t h, const FrameTarget& t) {
    if (!src || !camera_geometry_supported(w, h) || !t.valid()) return false;
    const uint8_t* u = src + static_cast<size_t>(w) * h;
    const uint8_t* v = u + static_cast<size_t>(w / 2) * (h / 2);
    blit_plane(src, w, h, t.y, t.width, t.height);
    blit_plane(u, w / 2, h / 2, t.cb, t.width / 2, t.height / 2);
    blit_plane(v, w / 2, h / 2, t.cr, t.width / 2, t.height / 2);
    return true;
}
} // namespace recam
