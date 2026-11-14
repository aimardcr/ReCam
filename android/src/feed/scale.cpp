#include "scale.hpp"

#include <string.h>
#include <vector>
#include <utility>
#if defined(__ARM_NEON)
#include <arm_neon.h>
#endif

namespace recam {

namespace {

// Bilinear interpolation uses 16.16 weights and two truncation steps.
// Horizontal rows are reused across vertically adjacent destination rows.
void resample(const SrcPlane& s, int32_t sw, int32_t sh,
              uint8_t* dst, int32_t dw, int32_t dh)
{
    const uint32_t xr = dw > 1 ? (static_cast<uint32_t>(sw - 1) << 16) / (dw - 1) : 0;
    const uint32_t yr = dh > 1 ? (static_cast<uint32_t>(sh - 1) << 16) / (dh - 1) : 0;
    struct Sample { int32_t a, b, fraction; };
    std::vector<Sample> samples(dw);
    for (int32_t x = 0; x < dw; ++x) {
        const uint32_t sx = static_cast<uint32_t>(x) * xr;
        const int32_t x0 = sx >> 16;
        const int32_t x1 = x0 + 1 < sw ? x0 + 1 : x0;
        samples[x] = {x0 * s.pixelStride, x1 * s.pixelStride, static_cast<int32_t>(sx & 65535)};
    }
    std::vector<uint8_t> rows[2] = {std::vector<uint8_t>(dw), std::vector<uint8_t>(dw)};
    int32_t rowIndex[2] = {-1, -1};
    auto horizontal = [&](int32_t y, int slot) {
        if (rowIndex[slot] == y) return;
        const uint8_t* in = s.data + static_cast<size_t>(y) * s.rowStride;
        uint8_t* out = rows[slot].data();
        if (sw == dw && s.pixelStride == 1) memcpy(out, in, dw);
        else for (int32_t x = 0; x < dw; ++x) {
            const auto& q = samples[x];
            const int32_t a = in[q.a], b = in[q.b];
            out[x] = static_cast<uint8_t>(a + (((b - a) * q.fraction) >> 16));
        }
        rowIndex[slot] = y;
    };
    for (int32_t y = 0; y < dh; ++y) {
        const uint32_t sy = static_cast<uint32_t>(y) * yr;
        const int32_t y0 = sy >> 16, y1 = y0 + 1 < sh ? y0 + 1 : y0;
        const int32_t fy = sy & 65535;
        if (rowIndex[1] == y0) {
            std::swap(rows[0], rows[1]);
            std::swap(rowIndex[0], rowIndex[1]);
        }
        horizontal(y0, 0);
        uint8_t* out = dst + static_cast<size_t>(y) * dw;
        if (!fy || y0 == y1) { memcpy(out, rows[0].data(), dw); continue; }
        horizontal(y1, 1);
        const uint8_t* top = rows[0].data();
        const uint8_t* bot = rows[1].data();
        int32_t x = 0;
#if defined(__ARM_NEON)
        for (; x + 8 <= dw; x += 8) {
            const uint16x8_t a = vmovl_u8(vld1_u8(top + x));
            const uint16x8_t b = vmovl_u8(vld1_u8(bot + x));
            const int16x8_t delta = vreinterpretq_s16_u16(vsubq_u16(b, a));
            const int32x4_t lo = vshrq_n_s32(vmulq_n_s32(vmovl_s16(vget_low_s16(delta)), fy), 16);
            const int32x4_t hi = vshrq_n_s32(vmulq_n_s32(vmovl_s16(vget_high_s16(delta)), fy), 16);
            const int16x8_t blended = vaddq_s16(vreinterpretq_s16_u16(a),
                                               vcombine_s16(vmovn_s32(lo), vmovn_s32(hi)));
            vst1_u8(out + x, vmovn_u16(vreinterpretq_u16_s16(blended)));
        }
#endif
        for (; x < dw; ++x) out[x] = static_cast<uint8_t>(top[x] + (((bot[x] - top[x]) * fy) >> 16));
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
    if (src.width < 2 || src.height < 2 || src.width > 16384 || src.height > 16384 ||
        dstW < 2 || dstH < 2 || dstW > 16384 || dstH > 16384 ||
        ((src.width | src.height | dstW | dstH) & 1)) return false;
    for (const auto& plane : {src.y, src.cb, src.cr})
        if (plane.rowStride <= 0 || plane.pixelStride < 1 || plane.pixelStride > 4) return false;
    if (static_cast<int64_t>(src.y.rowStride) < static_cast<int64_t>(src.width - 1) * src.y.pixelStride + 1 ||
        static_cast<int64_t>(src.cb.rowStride) < static_cast<int64_t>(src.width / 2 - 1) * src.cb.pixelStride + 1 ||
        static_cast<int64_t>(src.cr.rowStride) < static_cast<int64_t>(src.width / 2 - 1) * src.cr.pixelStride + 1)
        return false;

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

    if (cw == dw && ch == dh) {
        copy_plane(py, y, dw, dh);
        copy_plane(pu, u, dcw, dch);
        copy_plane(pv, v, dcw, dch);
        return true;
    }

    resample(py, cw,     ch,     y, dw,  dh);
    resample(pu, cw / 2, ch / 2, u, dcw, dch);
    resample(pv, cw / 2, ch / 2, v, dcw, dch);
    return true;
}

}  // namespace recam
