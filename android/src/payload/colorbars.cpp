// Hardcoded colour bars, written directly into a locked camera buffer.

#include "framesource.hpp"

#include <android/log.h>
#include <string.h>

#define LOG_TAG "ReCam"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

namespace recam {

namespace {

// BT.601 full range, matching the streams' HAL_DATASPACE_V0_JFIF.
struct Bar { uint8_t y, cb, cr; };

constexpr int kBarCount = 8;
constexpr Bar kBars[kBarCount] = {
    { 255, 128, 128 },  // white
    { 226,   0, 149 },  // yellow
    { 179, 171,   0 },  // cyan
    { 150,  44,  21 },  // green
    { 105, 212, 235 },  // magenta
    {  76,  85, 255 },  // red
    {  29, 255, 107 },  // blue
    {   0, 128, 128 },  // black
};

// Rebuilt only when the geometry changes; lives in .bss, never allocated.
constexpr int kMaxWidth = 8192;

struct RowCache {
    int32_t width = 0;
    uint8_t y[kMaxWidth];
    uint8_t cb[kMaxWidth / 2];
    uint8_t cr[kMaxWidth / 2];
};

RowCache g_rows;

void build_rows(int32_t width)
{
    if (g_rows.width == width) return;

    for (int32_t x = 0; x < width; x++)
        g_rows.y[x] = kBars[(x * kBarCount) / width].y;

    const int32_t cw = width / 2;
    for (int32_t x = 0; x < cw; x++) {
        // Chroma is half resolution: sample the bar at the corresponding luma column.
        const Bar& b = kBars[((x * 2) * kBarCount) / width];
        g_rows.cb[x] = b.cb;
        g_rows.cr[x] = b.cr;
    }

    g_rows.width = width;
}

// Strided write leaves an interleaved neighbour untouched.
inline void blit_row(uint8_t* dst, const uint8_t* src, int32_t count, int32_t pixelStride)
{
    if (pixelStride == 1) {
        memcpy(dst, src, static_cast<size_t>(count));
        return;
    }
    for (int32_t i = 0; i < count; i++)
        dst[i * pixelStride] = src[i];
}

class ColorBars final : public FrameSource {
public:
    const char* name() const override { return "colorbars"; }

    bool fill(const FrameTarget& t) override
    {
        if (!t.valid()) return false;
        if (t.width > kMaxWidth) return false;

        build_rows(t.width);

        // Luma: full resolution.
        for (int32_t row = 0; row < t.height; row++)
            blit_row(t.y.data + static_cast<ptrdiff_t>(row) * t.y.rowStride,
                     g_rows.y, t.width, t.y.pixelStride);

        // Chroma: 4:2:0, so half the columns and half the rows.
        const int32_t cw = t.width / 2;
        const int32_t ch = t.height / 2;
        for (int32_t row = 0; row < ch; row++) {
            blit_row(t.cb.data + static_cast<ptrdiff_t>(row) * t.cb.rowStride,
                     g_rows.cb, cw, t.cb.pixelStride);
            blit_row(t.cr.data + static_cast<ptrdiff_t>(row) * t.cr.rowStride,
                     g_rows.cr, cw, t.cr.pixelStride);
        }
        return true;
    }
};

ColorBars g_colorbars;

}  // namespace

FrameSource* colorbars_source() { return &g_colorbars; }

}  // namespace recam
