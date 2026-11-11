#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace recam::qcom {

// LP64 Qualcomm gralloc handle used by LE2115_13.1.0.592(EX01).
// Do not interpret other vendors' handles or newer handle revisions as this ABI.
// Field offsets also match QTI's gralloc/gr_priv_handle.h (pack(4)).
struct Handle {
    int fd = -1;
    uint32_t size = 0;
    int width = 0, height = 0;
    int requestedWidth = 0, requestedHeight = 0;
    int format = 0;
    uint32_t flags = 0;
};

template <typename T> inline T field(const void* p, size_t offset) {
    T v;
    memcpy(&v, static_cast<const uint8_t*>(p) + offset, sizeof(v));
    return v;
}

inline bool read_handle(const void* p, Handle& out) {
    if (!p || field<int>(p, 0) != 12 || field<int>(p, 4) != 2 ||
        field<int>(p, 8) != 23) return false;
    if (field<uint32_t>(p, 20) != 0x676d736d || field<uint32_t>(p, 52) != 1 ||
        field<uint32_t>(p, 76) != 0) return false;
    // Secure/protected buffers, secure display, and nonzero allocation offsets
    // are outside this backend's contract.
    const auto flags = field<uint32_t>(p, 24);
    if (flags & (0x400u | 0x4000u | 0x1000000u)) return false;
    if (field<uint64_t>(p, 64) & (1ull << 14)) return false;
    out = {field<int>(p, 12), field<uint32_t>(p, 72),
           field<int>(p, 28), field<int>(p, 32),
           field<int>(p, 36), field<int>(p, 40), field<int>(p, 44), flags};
    return out.fd >= 0 && out.size > 0 && out.size <= 64u * 1024 * 1024;
}

struct Layout {
    uint32_t stride = 0, yRows = 0, uvRows = 0;
    uint32_t yMeta = 0, yBytes = 0, uvMeta = 0, uvBytes = 0;
    uint32_t uvOffset = 0, bytes = 0, linearBytes = 0;
};

inline uint32_t align(uint32_t v, uint32_t a) { return (v + a - 1) & ~(a - 1); }

// Progressive 8-bit NV12 UBWC only. Verified against GetYuvUbwcSPPlaneInfo in
// this firmware; 10-bit, interlaced and custom camera layouts are not accepted.
inline bool layout(int width, int height, Layout& out) {
    if (width < 2 || height < 2 || width > 4096 || height > 4096 ||
        (width & 1) || (height & 1)) return false;
    const uint32_t w = width, h = height;
    out.stride = align(w, 128);
    out.yRows = align(h, 32);
    out.uvRows = align(h / 2, 32);
    out.yMeta = align(align((w + 31) / 32, 64) * align((h + 7) / 8, 16), 4096);
    out.uvMeta = align(align((w / 2 + 15) / 16, 64) * align((h / 2 + 7) / 8, 16), 4096);
    out.yBytes = align(out.stride * out.yRows, 4096);
    out.uvBytes = align(out.stride * out.uvRows, 4096);
    out.uvOffset = out.yMeta + out.yBytes; // start of UV metadata, not UV pixels
    out.bytes = out.uvOffset + out.uvMeta + out.uvBytes;
    out.linearBytes = out.yBytes + out.uvBytes;
    return true;
}

inline bool matches(const Handle& h, int w, int height, const Layout& l) {
    return h.format == 0x7fa30c06 && (h.flags & 0x08000000u) &&
           h.width == static_cast<int>(l.stride) && h.height == static_cast<int>(l.yRows) &&
           h.requestedWidth == w && h.requestedHeight == height && h.size >= l.bytes;
}

} // namespace recam::qcom
