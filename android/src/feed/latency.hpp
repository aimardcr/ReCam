#pragma once

#include <cstdint>
#include <deque>

namespace recam {

// CLOCK_MONOTONIC timestamps; wireOffset identifies the first VCL NAL byte.
struct FrameTiming {
    uint64_t wireOffset = 0;
    uint64_t receivedNs = 0;
    uint64_t queuedNs = 0;
    uint64_t decodedNs = 0;
    uint64_t copiedNs = 0;
};

// Preserve receive times across partial NALs and erases of the input buffer.
class ByteTimeline {
    struct Span { uint64_t end, ns; };
    std::deque<Span> spans;
public:
    uint64_t received = 0;
    uint64_t consumed = 0;

    void append(uint64_t bytes, uint64_t ns) {
        received += bytes;
        spans.push_back({received, ns});
    }
    uint64_t at(uint64_t offset) const {
        for (const auto& span : spans)
            if (offset < span.end) return span.ns;
        return 0;
    }
    void discard(uint64_t bytes) {
        consumed += bytes;
        while (!spans.empty() && spans.front().end <= consumed) spans.pop_front();
    }
};

} // namespace recam
