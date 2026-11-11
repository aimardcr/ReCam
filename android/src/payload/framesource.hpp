// What produces the pixels we write into an intercepted camera buffer.

#pragma once

#include <stdint.h>
#include "../common/frame_geometry.hpp"

namespace recam {

// A locked destination buffer, described by what the gralloc driver reported.
struct FrameTarget {
    struct Plane {
        uint8_t* data       = nullptr;
        int32_t  rowStride  = 0;  // bytes between rows, NOT width
        int32_t  pixelStride = 0; // bytes between samples; 2 means semi-planar (NV12/NV21)
    };

    int32_t width  = 0;
    int32_t height = 0;
    int32_t format = 0;   // HAL_PIXEL_FORMAT_*

    Plane y, cb, cr;

    bool valid() const
    {
        if (!camera_geometry_supported(static_cast<uint32_t>(width), static_cast<uint32_t>(height)))
            return false;
        const auto plane_ok = [](const Plane& p, int columns) {
            return p.data && p.pixelStride >= 1 && p.pixelStride <= 4 &&
                   p.rowStride > 0 && static_cast<int64_t>(p.rowStride) >=
                       static_cast<int64_t>(columns - 1) * p.pixelStride + 1;
        };
        return plane_ok(y, width) && plane_ok(cb, width / 2) && plane_ok(cr, width / 2);
    }
};

class FrameSource {
public:
    virtual ~FrameSource() {}

    virtual const char* name() const = 0;

    // False means leave the real frame alone.
    virtual bool fill(const FrameTarget& t) = 0;
};

// SMPTE-style colour bars, BT.601 full range.
FrameSource* colorbars_source();

}  // namespace recam
