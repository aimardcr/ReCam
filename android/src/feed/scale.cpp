#include "scale.hpp"

#include <string.h>

namespace recam {

namespace {

// 16.16 fixed point; a 65535 fraction times a 255 delta still fits in int32.
void resample(const SrcPlane& s, int32_t sw, int32_t sh,
              uint8_t* dst, int32_t dw, int32_t dh)
{
    const uint32_t xRatio = (dw > 1) ? ((static_cast<uint32_t>(sw - 1) << 16) / (dw - 1)) : 0;
    const uint32_t yRatio = (dh > 1) ? ((static_cast<uint32_t>(sh - 1) << 16) / (dh - 1)) : 0;

    for (int32_t y = 0; y < dh; y++) {
        const uint32_t sy = static_cast<uint32_t>(y) * yRatio;
        const int32_t  y0 = static_cast<int32_t>(sy >> 16);
        const int32_t  y1 = (y0 + 1 < sh) ? y0 + 1 : y0;
        const int32_t  fy = static_cast<int32_t>(sy & 0xFFFF);

        const uint8_t* r0 = s.data + static_cast<size_t>(y0) * s.rowStride;
        const uint8_t* r1 = s.data + static_cast<size_t>(y1) * s.rowStride;
        uint8_t* o = dst + static_cast<size_t>(y) * dw;

        uint32_t sx = 0;
        for (int32_t x = 0; x < dw; x++, sx += xRatio) {
            const int32_t x0 = static_cast<int32_t>(sx >> 16);
            const int32_t x1 = (x0 + 1 < sw) ? x0 + 1 : x0;
            const int32_t fx = static_cast<int32_t>(sx & 0xFFFF);

            const int32_t a = r0[x0 * s.pixelStride];
            const int32_t b = r0[x1 * s.pixelStride];
            const int32_t c = r1[x0 * s.pixelStride];
            const int32_t d = r1[x1 * s.pixelStride];

            const int32_t top = a + (((b - a) * fx) >> 16);
            const int32_t bot = c + (((d - c) * fx) >> 16);
            o[x] = static_cast<uint8_t>(top + (((bot - top) * fy) >> 16));
        }
    }
}

void copy_plane(const SrcPlane& s, uint8_t* dst, int32_t w, int32_t h)
{
    if (s.pixelStride == 1 && s.rowStride == w) {
        memcpy(dst, s.data, static_cast<size_t>(w) * h);
        return;
    }
    for (int32_t y = 0; y < h; y++) {
        const uint8_t* in = s.data + static_cast<size_t>(y) * s.rowStride;
        uint8_t* out = dst + static_cast<size_t>(y) * w;
        if (s.pixelStride == 1) {
            memcpy(out, in, static_cast<size_t>(w));
        } else {
            for (int32_t x = 0; x < w; x++) out[x] = in[x * s.pixelStride];
        }
    }
}

}  // namespace

bool scale_to_i420(const SrcFrame& src, uint8_t* dst, uint32_t dstW, uint32_t dstH)
{
    if (!dst || !src.y.data || !src.cb.data || !src.cr.data) return false;
    if (src.width < 2 || src.height < 2 || dstW < 2 || dstH < 2) return false;

    const int32_t dw = static_cast<int32_t>(dstW), dh = static_cast<int32_t>(dstH);
    const int32_t dcw = dw / 2, dch = dh / 2;

    uint8_t* y = dst;
    uint8_t* u = y + static_cast<size_t>(dw) * dh;
    uint8_t* v = u + static_cast<size_t>(dcw) * dch;

    if (dw == src.width && dh == src.height) {
        copy_plane(src.y,  y, dw,  dh);
        copy_plane(src.cb, u, dcw, dch);
        copy_plane(src.cr, v, dcw, dch);
        return true;
    }

    // Centre-crop, not letterbox: black bars would announce the substitution.
    int32_t cw = src.width, ch = src.height;
    if (static_cast<int64_t>(src.width) * dh > static_cast<int64_t>(dw) * src.height)
        cw = static_cast<int32_t>(static_cast<int64_t>(src.height) * dw / dh);
    else
        ch = static_cast<int32_t>(static_cast<int64_t>(src.width) * dh / dw);

    // Even, so the crop lands on a chroma sample rather than between two.
    cw &= ~1; ch &= ~1;
    if (cw < 2 || ch < 2) return false;
    const int32_t cx = ((src.width - cw) / 2) & ~1;
    const int32_t cy = ((src.height - ch) / 2) & ~1;

    SrcPlane py = src.y, pu = src.cb, pv = src.cr;
    py.data += static_cast<size_t>(cy) * py.rowStride + static_cast<size_t>(cx) * py.pixelStride;
    pu.data += static_cast<size_t>(cy / 2) * pu.rowStride + static_cast<size_t>(cx / 2) * pu.pixelStride;
    pv.data += static_cast<size_t>(cy / 2) * pv.rowStride + static_cast<size_t>(cx / 2) * pv.pixelStride;

    resample(py, cw,     ch,     y, dw,  dh);
    resample(pu, cw / 2, ch / 2, u, dcw, dch);
    resample(pv, cw / 2, ch / 2, v, dcw, dch);
    return true;
}

}  // namespace recam
