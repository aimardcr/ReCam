// Resampling a decoded YUV 4:2:0 frame into the ring's canonical packed I420.

#pragma once

#include <stdint.h>

namespace recam {

// Mirrors what AImage reports, so a semi-planar decoder needs no special case.
struct SrcPlane {
    const uint8_t* data        = nullptr;
    int32_t        rowStride   = 0;
    int32_t        pixelStride = 1;
};

struct SrcFrame {
    SrcPlane y, cb, cr;
    int32_t  width  = 0;
    int32_t  height = 0;
};

// Writes exactly dstW x dstH packed I420. dstW and dstH must be even.
bool scale_to_i420(const SrcFrame& src, uint8_t* dst, uint32_t dstW, uint32_t dstH);

}  // namespace recam
