#include "jpeg_frame.hpp"
#include "../common/ring_access.hpp"
#include "../../third_party/jpge/jpge.h"

#include <cstdlib>
#include <cstring>
#include <initializer_list>

namespace recam {
namespace {
struct BlobFooter { uint16_t id, padding; uint32_t size; };
static_assert(sizeof(BlobFooter) == 8 && offsetof(BlobFooter, size) == 4);
constexpr uint16_t kJpegBlob = 0xff;
uint32_t g_access = 0;

struct Bytes {
    uint8_t* p;
    explicit Bytes(size_t n) : p(static_cast<uint8_t*>(malloc(n))) {}
    ~Bytes() { free(p); }
    Bytes(const Bytes&) = delete;
    Bytes& operator=(const Bytes&) = delete;
};

uint16_t be16(const uint8_t* p) { return uint16_t(p[0]) * 256 + p[1]; }

bool exif_orientation(const uint8_t* p, size_t n, uint16_t& orientation) {
    if (n < 8) return false;
    const bool le = p[0] == 'I' && p[1] == 'I';
    if (!le && !(p[0] == 'M' && p[1] == 'M')) return false;
    const auto u16 = [le](const uint8_t* v) -> uint16_t {
        return le ? uint16_t(v[1]) * 256 + v[0] : be16(v);
    };
    const auto u32 = [le](const uint8_t* v) -> uint32_t {
        return le ? uint32_t(v[0]) | uint32_t(v[1]) << 8 | uint32_t(v[2]) << 16 | uint32_t(v[3]) << 24
                  : uint32_t(v[3]) | uint32_t(v[2]) << 8 | uint32_t(v[1]) << 16 | uint32_t(v[0]) << 24;
    };
    if (u16(p + 2) != 42) return false;
    const size_t offset = u32(p + 4);
    if (offset < 8 || offset > n || n - offset < 6) return false;
    const uint16_t count = u16(p + offset);
    if (count > (n - offset - 6) / 12) return false;
    bool found = false;
    for (size_t i = 0; i < count; ++i) {
        const auto* entry = p + offset + 2 + i * 12;
        if (u16(entry) != 0x112) continue;
        if (found || u16(entry + 2) != 3 || u32(entry + 4) != 1) return false;
        orientation = u16(entry + 8);
        if (orientation < 1 || orientation > 8) return false;
        found = true;
    }
    return true;
}

struct JpegInfo { uint32_t width = 0, height = 0; uint16_t orientation = 1; };
bool jpeg_info(const uint8_t* p, size_t n, JpegInfo& info) {
    if (n < 4 || p[0] != 0xff || p[1] != 0xd8 || p[n - 2] != 0xff || p[n - 1] != 0xd9)
        return false;
    bool sof = false, exif = false;
    size_t pos = 2;
    for (unsigned segments = 0; segments < 1024 && pos < n; ++segments) {
        if (p[pos++] != 0xff) return false;
        while (pos < n && p[pos] == 0xff) ++pos;
        if (pos == n) return false;
        const uint8_t marker = p[pos++];
        if (marker == 0 || marker == 0xd8 || marker == 0xd9 || marker == 1 ||
            (marker >= 0xd0 && marker <= 0xd7) || n - pos < 2) return false;
        const size_t len = be16(p + pos);
        if (len < 2 || len > n - pos) return false;
        const uint8_t* data = p + pos + 2;
        const size_t bytes = len - 2;
        if (marker == 0xe1 && bytes >= 6 && !memcmp(data, "Exif\0\0", 6)) {
            if (exif || !exif_orientation(data + 6, bytes - 6, info.orientation)) return false;
            exif = true;
        }
        if (marker == 0xc0 || marker == 0xc1 || marker == 0xc2) {
            if (sof || bytes < 6 || data[0] != 8 || (data[5] != 1 && data[5] != 3) ||
                bytes != size_t(6 + 3 * data[5])) return false;
            info.height = be16(data + 1); info.width = be16(data + 3);
            sof = true;
        }
        if (marker == 0xda) return sof && camera_geometry_supported(info.width, info.height);
        pos += len;
    }
    return false;
}

class Output final : public jpge::output_stream {
public:
    uint8_t* data;
    size_t capacity, size = 0;
    Output(uint8_t* p, size_t n) : data(p), capacity(n) {}
    bool put_buf(const void* p, int n) override {
        if (n < 0 || static_cast<size_t>(n) > capacity - size) return false;
        memcpy(data + size, p, n); size += n;
        return true;
    }
};

uint8_t clamp(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }

bool encode(const FrameTarget& t, uint8_t* rgb, Output& output, int quality) {
    jpge::params params;
    params.m_quality = quality;
    jpge::jpeg_encoder encoder;
    if (!encoder.init(&output, t.width, t.height, 3, params)) return false;
    for (int row = 0; row < t.height; ++row) {
        const auto* y = t.y.data + size_t(row) * t.y.rowStride;
        const auto* u = t.cb.data + size_t(row / 2) * t.cb.rowStride;
        const auto* v = t.cr.data + size_t(row / 2) * t.cr.rowStride;
        for (int x = 0; x < t.width; ++x) {
            const int cb = u[x / 2] - 128, cr = v[x / 2] - 128;
            // FrameSource pixels use full-range BT.601, matching JPEG/JFIF.
            rgb[x * 3] = clamp(y[x] + ((91881 * cr + 32768) >> 16));
            rgb[x * 3 + 1] = clamp(y[x] - ((22554 * cb + 46802 * cr + 32768) >> 16));
            rgb[x * 3 + 2] = clamp(y[x] + ((116130 * cb + 32768) >> 16));
        }
        if (!encoder.process_scanline(rgb)) return false;
    }
    return encoder.process_scanline(nullptr);
}
}

FrameSource* jpeg_frame_fill(uint8_t* blob, size_t capacity, uint32_t width, uint32_t height,
                             FrameSource* source, FrameSource* fallback) {
    RingWrite guard(&g_access);
    if (!guard || !blob || capacity < 1024 || capacity > 64u * 1024 * 1024 ||
        !camera_geometry_supported(width, height)) return nullptr;
    BlobFooter footer{};
    memcpy(&footer, blob + capacity - sizeof(footer), sizeof(footer));
    const size_t available = capacity - sizeof(footer);
    if (footer.id != kJpegBlob || footer.size > available) return nullptr;
    JpegInfo info;
    if (!jpeg_info(blob, footer.size, info) ||
        !((info.width == width && info.height == height) ||
          (info.width == height && info.height == width))) return nullptr;

    const size_t pixels = size_t(info.width) * info.height;
    Bytes yuv(pixels + pixels / 2), rgb(info.width * 3), compressed(available);
    if (!yuv.p || !rgb.p || !compressed.p) return nullptr;
    FrameTarget t;
    t.width = info.width; t.height = info.height; t.format = 0x23;
    t.y = {yuv.p, t.width, 1};
    t.cb = {yuv.p + pixels, t.width / 2, 1};
    t.cr = {yuv.p + pixels + pixels / 4, t.width / 2, 1};
    FrameSource* used = nullptr;
    if (source && source->snapshot(t)) used = source;
    else if (fallback && fallback->snapshot(t)) used = fallback;
    if (!used) return nullptr;

    // Rebuild EXIF without the original scene's thumbnail or maker-note images.
    uint8_t exif[] = {0xff,0xe1,0,34,'E','x','i','f',0,0,'I','I',42,0,8,0,0,0,
                     1,0,0x12,1,3,0,1,0,0,0,1,0,0,0,0,0,0,0};
    exif[28] = static_cast<uint8_t>(info.orientation);
    size_t length = 0;
    for (int quality : {85, 60, 35}) {
        Output output(compressed.p, available - sizeof(exif));
        if (encode(t, rgb.p, output, quality)) { length = output.size; break; }
    }
    if (length < 4) return nullptr;
    memmove(compressed.p + 2 + sizeof(exif), compressed.p + 2, length - 2);
    memcpy(compressed.p + 2, exif, sizeof(exif));
    length += sizeof(exif);
    // Commit only after a complete JPEG fits; never expose a partially encoded image.
    memcpy(blob, compressed.p, length);
    memset(blob + length, 0, available - length);
    footer = {kJpegBlob, 0, static_cast<uint32_t>(length)};
    memcpy(blob + available, &footer, sizeof(footer));
    return used;
}
}
