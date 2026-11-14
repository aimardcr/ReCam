#pragma once
#include "framesource.hpp"
#include <cstddef>

namespace recam {
// Replace a complete camera JPEG BLOB. Failures leave its bytes unchanged.
FrameSource* jpeg_frame_fill(uint8_t* blob, size_t capacity, uint32_t width, uint32_t height,
                             FrameSource* source, FrameSource* fallback);
}
