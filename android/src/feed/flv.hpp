// Narrow FLV/AVC transport profile: one complete, non-reordered picture per tag.
#pragma once
#include "h264.hpp"
#include <cstring>
#include <vector>

namespace recam::flv {
constexpr size_t kLimit = 8 * 1024 * 1024;
enum class Result { More, Ready, Invalid };
inline uint32_t be(const uint8_t* p, unsigned n) {
    uint32_t v = 0;
    while (n--) v = (v << 8) | *p++;
    return v;
}
inline Result header(const uint8_t* p, size_t n, size_t& consumed) {
    if (n < 9) return Result::More;
    const size_t len = be(p + 5, 4);
    if (memcmp(p, "FLV", 3) || p[3] != 1 || (p[4] & ~5) || !(p[4] & 1) ||
        len < 9 || len > 4096) return Result::Invalid;
    if (n < len + 4) return Result::More;
    if (be(p + len, 4)) return Result::Invalid;
    consumed = len + 4;
    return Result::Ready;
}
struct Tag { uint8_t type; const uint8_t* data; size_t size, bytes; };
inline Result tag(const uint8_t* p, size_t n, Tag& out) {
    if (n < 11) return Result::More;
    const size_t size = be(p + 1, 3);
    if ((p[0] != 8 && p[0] != 9 && p[0] != 18) || be(p + 8, 3) || size > kLimit - 15)
        return Result::Invalid;
    if (n < size + 15) return Result::More;
    if (be(p + 11 + size, 4) != size + 11) return Result::Invalid;
    out = {p[0], p + 11, size, size + 15};
    return Result::Ready;
}
inline bool config(const uint8_t* p, size_t n, unsigned& lengthBytes,
                   std::vector<uint8_t>& sps, std::vector<uint8_t>& pps) {
    if (n < 8 || p[0] != 1 || (p[4] & 0xfc) != 0xfc || (p[5] & 0xe0) != 0xe0 ||
        (p[5] & 31) != 1) return false;
    const unsigned width = (p[4] & 3) + 1;
    if (width == 3) return false; // reserved by AVCDecoderConfigurationRecord
    size_t pos = 6, size = be(p + pos, 2);
    pos += 2;
    if (!size || size > n - pos || (p[pos] & 31) != h264::kNalSps) return false;
    std::vector<uint8_t> a(p + pos, p + pos + size);
    pos += size;
    if (pos >= n || p[pos++] != 1 || n - pos < 2) return false;
    size = be(p + pos, 2); pos += 2;
    if (!size || size > n - pos || (p[pos] & 31) != h264::kNalPps) return false;
    std::vector<uint8_t> b(p + pos, p + pos + size);
    sps = std::move(a); pps = std::move(b); lengthBytes = width;
    return true;
}
inline bool picture(const uint8_t* p, size_t n, unsigned width,
                    std::vector<uint8_t>& annexb, size_t& firstVcl) {
    if (width != 1 && width != 2 && width != 4) return false;
    annexb.clear();
    bool vcl = false;
    for (size_t pos = 0; pos < n;) {
        if (n - pos < width) return false;
        const size_t size = be(p + pos, width); pos += width;
        if (!size || size > n - pos || annexb.size() + 4 + size > kLimit) return false;
        const uint8_t type = p[pos] & 31;
        if (p[pos] & 0x80) return false;
        if (h264::is_vcl(type)) {
            const bool first = h264::starts_picture(p + pos, size);
            if ((!vcl && !first) || (vcl && first)) return false;
            if (!vcl) firstVcl = pos;
            vcl = true;
        }
        annexb.insert(annexb.end(), {0, 0, 0, 1});
        annexb.insert(annexb.end(), p + pos, p + pos + size);
        pos += size;
    }
    return vcl;
}
} // namespace recam::flv
