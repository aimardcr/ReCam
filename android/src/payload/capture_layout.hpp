#pragma once

#include <cstdint>
#include "framesource.hpp"

namespace recam {

bool capture_layout_install(uintptr_t cameraFunction, uintptr_t replacement, uintptr_t vendorSetter);
bool capture_layout_remove();
uint32_t capture_layout_hook_count();
void capture_layout_set_vendor(void* metadata, uint64_t vendor, bool observe);
int capture_layout_query(const void* metadata, uint32_t tag, void* entry, bool observe);
// Adjusts only a matching, verified NV21 allocation; all other layouts stay intact.
bool capture_layout_apply(int64_t timestamp, const void* nativeHandle, FrameTarget& target);

} // namespace recam
