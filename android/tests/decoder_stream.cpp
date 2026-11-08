// Real MediaCodec byte-buffer integration, without cameraserver or gralloc.
#include "../src/feed/decoder.hpp"
#include <fcntl.h>
#include <unistd.h>
#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdio>

int main(int argc, char** argv) {
    assert(argc == 3); // matching 320x240 FLV and Annex-B fixtures
    for (int i = 1; i < argc; ++i) {
        recam::VideoDecoder decoder;
        const int fd = open(argv[i], O_RDONLY);
        assert(fd >= 0 && decoder.open_stream(fd)); // decoder owns fd
        unsigned frames = 0;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
        while (!decoder.stream_ended() && std::chrono::steady_clock::now() < deadline) {
            recam::SrcFrame frame;
            if (decoder.next(&frame)) {
                assert(frame.width == 320 && frame.height == 240);
                int low = 255, high = 0;
                for (int y = 0; y < frame.height; y += 8)
                    for (int x = 0; x < frame.width; x += 8) {
                        const int value = frame.y.data[y * frame.y.rowStride + x * frame.y.pixelStride];
                        low = std::min(low, value); high = std::max(high, value);
                    }
                assert(high - low > 40); // actual testsrc pixels, not empty buffers
                ++frames;
                decoder.release();
            }
            decoder.wait_stream(2);
        }
        assert(decoder.stream_ended() && frames > 0);
        decoder.close();
        printf("%s: %u decoded frames with valid pixels; stream ended\n", argv[i], frames);
    }
    fflush(nullptr);
    _exit(0); // matches production feed's explicit cleanup / platform exit boundary
}
