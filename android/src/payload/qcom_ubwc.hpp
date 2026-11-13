#pragma once

#include "framesource.hpp"

namespace recam {

constexpr int32_t kQcomNv12Ubwc = 0x7fa30c06;

// Only the verified Qualcomm handle/layout is accepted. Conversion uses owned
// staging buffers; the camera allocation is written only after GPU completion.
FrameSource* qcom_ubwc_fill(const void* handle, int width, int height, int fence,
                          FrameSource* source, FrameSource* fallback);

// Called after camera callbacks drain. Never waits for GPU completion.
void qcom_ubwc_release();

// Permit one new initialization attempt on a real callback after Start.
void qcom_ubwc_retry_initialization();

} // namespace recam
