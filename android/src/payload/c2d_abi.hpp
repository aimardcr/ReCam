#pragma once

#include <cstddef>
#include <cstdint>

// Minimal ABI declarations, not an implementation of Qualcomm C2D. Constants
// and layouts are documented in AOSP hardware/qcom/display msm8996/libcopybit/
// c2d2.h (android-7.1.2_r33) and checked against the OnePlus 9 ARM64 binaries.
namespace recam::qcom {

struct Surface {
    uint32_t format, width, height;
    void* plane0;
    void* gpu0;
    int32_t stride0;
    void* plane1;
    void* gpu1;
    int32_t stride1;
    void* plane2;
    void* gpu2;
    int32_t stride2;
};
struct Rect { int32_t x, y, width, height; };
struct Object {
    uint32_t source, foreground, background, palette, mask;
    Rect sourceRect, targetRect;
    int32_t originX, originY, rotation;
    Rect scissor;
    uint32_t maskSurface, alpha, colorKey;
    Object* next;
};
struct DriverInfo { uint32_t capabilities, workaround, reserved[3]; };
static_assert(sizeof(Surface) == 88 && offsetof(Surface, stride1) == 56);
static_assert(sizeof(Object) == 104 && offsetof(Object, next) == 96);
static_assert(sizeof(DriverInfo) == 20);

constexpr uint32_t kNv12 = 152;
constexpr uint32_t kUbwc = 1u << 23;
constexpr uint32_t kSource = 1, kTarget = 2;
constexpr uint32_t kHostYuvPhysical = 3 | (1 << 3);
constexpr uint32_t kCopyMask = (1 << 0) | (1 << 4) | (1 << 14) |
                               (1 << 15) | (1 << 25);
constexpr uint32_t kDmaBufMemory = 3;

struct C2d {
    int (*create)(uint32_t*, uint32_t, uint32_t, void*) = nullptr;
    int (*destroy)(uint32_t) = nullptr;
    int (*map)(int, void*, uint32_t, uint32_t, uint32_t, void**) = nullptr;
    int (*unmap)(void*) = nullptr;
    int (*draw)(uint32_t, uint32_t, Rect*, uint32_t, uint32_t, Object*, uint32_t) = nullptr;
    int (*flush)(uint32_t, void**) = nullptr;
    int (*fence)(uint32_t, void*, int32_t*) = nullptr;
    int (*finish)(uint32_t) = nullptr;
    int (*capabilities)(DriverInfo*) = nullptr;
};

} // namespace recam::qcom
