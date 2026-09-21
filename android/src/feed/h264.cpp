#include "h264.hpp"

namespace recam::h264 {

namespace {

// Reads RBSP bits, skipping the 0x03 an encoder inserts after two zero bytes.
class BitReader {
public:
    BitReader(const uint8_t* d, size_t n) : mData(d), mSize(n) {}

    bool bad() const { return mBad; }

    uint32_t u(int bits)
    {
        uint32_t v = 0;
        for (int i = 0; i < bits; i++) v = (v << 1) | u1();
        return v;
    }

    uint32_t u1()
    {
        if (mByte >= mSize) { mBad = true; return 0; }

        // Two zero bytes then 0x03: the 0x03 is not part of the payload.
        if (mBit == 0 && mByte >= 2 && mData[mByte] == 0x03 &&
            mData[mByte - 1] == 0 && mData[mByte - 2] == 0) {
            mByte++;
            if (mByte >= mSize) { mBad = true; return 0; }
        }

        const uint32_t v = (mData[mByte] >> (7 - mBit)) & 1u;
        if (++mBit == 8) { mBit = 0; mByte++; }
        return v;
    }

    uint32_t ue()
    {
        int zeros = 0;
        while (!u1()) {
            if (mBad || ++zeros > 31) { mBad = true; return 0; }
        }
        return zeros ? ((1u << zeros) - 1 + u(zeros)) : 0;
    }

    int32_t se()
    {
        const uint32_t k = ue();
        return (k & 1) ? static_cast<int32_t>((k + 1) / 2) : -static_cast<int32_t>(k / 2);
    }

private:
    const uint8_t* mData;
    size_t         mSize;
    size_t         mByte = 0;
    int            mBit  = 0;
    bool           mBad  = false;
};

bool is_high_profile(uint32_t p)
{
    switch (p) {
        case 100: case 110: case 122: case 244: case 44:
        case 83:  case 86:  case 118: case 128: case 138:
        case 139: case 134: case 135: return true;
        default: return false;
    }
}

// Values are discarded; only the bit count matters for what follows.
void skip_scaling_list(BitReader& r, int size)
{
    int32_t last = 8, next = 8;
    for (int i = 0; i < size && !r.bad(); i++) {
        if (next) next = (last + r.se() + 256) % 256;
        last = next ? next : last;
    }
}

}  // namespace

bool next_nal(const uint8_t* buf, size_t len, size_t* pos, Nal* out)
{
    size_t i = *pos;

    // Find the next start code.
    while (i + 3 <= len && !(buf[i] == 0 && buf[i + 1] == 0 && buf[i + 2] == 1)) i++;
    if (i + 3 > len) { *pos = len; return false; }

    const size_t start = i + 3;
    if (start >= len) { *pos = len; return false; }

    // Find the one after it; that is where this NAL ends.
    size_t j = start;
    while (j + 3 <= len && !(buf[j] == 0 && buf[j + 1] == 0 && buf[j + 2] == 1)) j++;

    size_t end = (j + 3 <= len) ? j : len;
    while (end > start && buf[end - 1] == 0) end--;   // trailing byte of a 4-byte code

    out->data = buf + start;
    out->size = end - start;
    out->type = buf[start] & 0x1f;

    *pos = (j + 3 <= len) ? j : len;
    return out->size > 0;
}

bool starts_picture(const uint8_t* nal, size_t len)
{
    if (!nal || len < 2 || !is_vcl(nal[0] & 0x1f)) return false;
    BitReader r(nal + 1, len - 1);
    const uint32_t firstMb = r.ue();
    return !r.bad() && firstMb == 0;
}

SpsInfo parse_sps(const uint8_t* nal, size_t len)
{
    SpsInfo info;
    if (!nal || len < 4 || (nal[0] & 0x1f) != kNalSps) return info;

    BitReader r(nal + 1, len - 1);   // past the NAL header byte

    const uint32_t profile = r.u(8);
    r.u(8);                          // constraint flags + reserved
    r.u(8);                          // level_idc
    r.ue();                          // seq_parameter_set_id

    uint32_t chromaFormat = 1;       // 4:2:0 unless the high-profile block says otherwise
    bool separateColourPlane = false;

    if (is_high_profile(profile)) {
        chromaFormat = r.ue();
        if (chromaFormat == 3) separateColourPlane = r.u1() != 0;
        r.ue();                      // bit_depth_luma_minus8
        r.ue();                      // bit_depth_chroma_minus8
        r.u1();                      // qpprime_y_zero_transform_bypass_flag
        if (r.u1()) {                // seq_scaling_matrix_present_flag
            const int lists = (chromaFormat != 3) ? 8 : 12;
            for (int i = 0; i < lists && !r.bad(); i++)
                if (r.u1()) skip_scaling_list(r, i < 6 ? 16 : 64);
        }
    }

    r.ue();                          // log2_max_frame_num_minus4
    const uint32_t pocType = r.ue();
    if (pocType == 0) {
        r.ue();                      // log2_max_pic_order_cnt_lsb_minus4
    } else if (pocType == 1) {
        r.u1();                      // delta_pic_order_always_zero_flag
        r.se();                      // offset_for_non_ref_pic
        r.se();                      // offset_for_top_to_bottom_field
        const uint32_t n = r.ue();
        for (uint32_t i = 0; i < n && !r.bad(); i++) r.se();
    }

    r.ue();                          // max_num_ref_frames
    r.u1();                          // gaps_in_frame_num_value_allowed_flag

    const uint32_t widthMbs   = r.ue() + 1;
    const uint32_t heightUnits = r.ue() + 1;
    const uint32_t frameMbsOnly = r.u1();
    if (!frameMbsOnly) r.u1();       // mb_adaptive_frame_field_flag
    r.u1();                          // direct_8x8_inference_flag

    uint32_t cropL = 0, cropR = 0, cropT = 0, cropB = 0;
    if (r.u1()) {                    // frame_cropping_flag
        cropL = r.ue(); cropR = r.ue(); cropT = r.ue(); cropB = r.ue();
    }

    if (r.bad()) return info;

    // Table 6-1: 4:2:0 subsamples both axes, 4:2:2 only the horizontal one.
    uint32_t subW = 1, subH = 1;
    if (!separateColourPlane) {
        if (chromaFormat == 1)      { subW = 2; subH = 2; }
        else if (chromaFormat == 2) { subW = 2; subH = 1; }
    }
    const uint32_t unitX = separateColourPlane ? 1 : subW;
    const uint32_t unitY = (separateColourPlane ? 1 : subH) * (2 - frameMbsOnly);

    const int32_t w = static_cast<int32_t>(widthMbs * 16 - unitX * (cropL + cropR));
    const int32_t h = static_cast<int32_t>((2 - frameMbsOnly) * heightUnits * 16 -
                                           unitY * (cropT + cropB));
    if (w < 2 || h < 2 || w > 16384 || h > 16384) return info;

    info.width  = w;
    info.height = h;
    info.ok     = true;
    return info;
}

}  // namespace recam::h264
