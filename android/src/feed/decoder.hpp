// Decoding an on-device video file into the ring, via MediaCodec and AImageReader.

#pragma once

#include <stdint.h>

#include "scale.hpp"

namespace recam {

class VideoDecoder {
public:
    ~VideoDecoder();

    bool open(const char* path);

    // Annex-B H.264 off a socket. Takes ownership of `fd`; geometry comes from the SPS.
    bool open_stream(int fd);

    void close();

    // Acquire the next frame; its planes stay valid until release(). Loops at EOS.
    bool next(SrcFrame* out);
    void release();

    // The peer hung up, so the caller should fall back to its other source.
    bool stream_ended() const;

    int32_t width()  const { return mWidth; }
    int32_t height() const { return mHeight; }
    int64_t frame_interval_us() const { return mIntervalUs; }

private:
    bool pump();
    bool pump_stream();
    bool queue_au();
    bool start_codec_from_csd();
    void rewind();

    struct Impl;
    Impl*   mImpl       = nullptr;
    int32_t mWidth      = 0;
    int32_t mHeight     = 0;
    int64_t mIntervalUs = 33333;
};

}  // namespace recam
