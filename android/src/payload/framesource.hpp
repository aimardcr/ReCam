// What produces the pixels we write into an intercepted camera buffer.

#pragma once

#include <stdint.h>

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
        return width > 1 && height > 1 &&
               y.data && cb.data && cr.data &&
               y.rowStride  >= width && y.pixelStride  >= 1 &&
               cb.rowStride > 0      && cb.pixelStride >= 1 &&
               cr.rowStride > 0      && cr.pixelStride >= 1;
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
