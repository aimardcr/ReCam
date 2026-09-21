// Enough H.264 to configure MediaCodec from a raw Annex-B stream.

#pragma once

#include <stddef.h>
#include <stdint.h>

namespace recam::h264 {

struct Nal {
    const uint8_t* data = nullptr;   // payload, start code stripped
    size_t         size = 0;
    uint8_t        type = 0;         // nal_unit_type; 7 is SPS, 8 is PPS
};

static const uint8_t kNalSps = 7;
static const uint8_t kNalPps = 8;
static const uint8_t kNalIdr = 5;

// Walk Annex-B start codes. `pos` carries the scan position between calls.
bool next_nal(const uint8_t* buf, size_t len, size_t* pos, Nal* out);

struct SpsInfo {
    int32_t width  = 0;
    int32_t height = 0;
    bool    ok     = false;
};

// Dimensions come from the SPS, so a stream needs no out-of-band geometry.
SpsInfo parse_sps(const uint8_t* nal, size_t len);

inline bool is_vcl(uint8_t type) { return type >= 1 && type <= 5; }

// first_mb_in_slice == 0, so this slice opens a picture rather than continuing one.
bool starts_picture(const uint8_t* nal, size_t len);

}  // namespace recam::h264
