// CPU-write locking of a camera gralloc buffer.

#pragma once

#include <stdint.h>

namespace recam {

struct LockedBuffer {
    void*       handle       = nullptr;  // AHardwareBuffer*, set by the NDK path
    const void* nativeHandle = nullptr;  // buffer_handle_t, set by the mapper path
    struct { uint8_t* data; int32_t rowStride; int32_t pixelStride; } plane[3]{};
};

// Resolve the lock API once; false means frame substitution is unavailable.
bool bufferlock_init();

// Is the android_ycbcr fallback available? Only it can describe IMPLEMENTATION_DEFINED.
bool bufferlock_have_ycbcr();

// `fenceFd` is the camera's release fence, -1 if none, and stays owned by the caller.
bool bufferlock_acquire(void* anwBuffer, int32_t fenceFd, LockedBuffer* out);

// For a caller holding an AHardwareBuffer; never pass an ANativeWindowBuffer here.
bool bufferlock_acquire_ahb(void* hwBuffer, int32_t fenceFd, LockedBuffer* out);

// Asks the gralloc driver for the layout, so it works where lockPlanes cannot.
bool bufferlock_acquire_handle(const void* nativeHandle, int32_t width, int32_t height,
                               int32_t fenceFd, LockedBuffer* out);

void bufferlock_release(LockedBuffer* b);

}  // namespace recam
