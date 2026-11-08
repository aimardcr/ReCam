// Test the production decoder against deterministic MediaCodec queues.
// Link with --wrap for the six buffer APIs below; no camera or real codec needed.
#include "../src/feed/scale.hpp"
#define private public
#include "../src/feed/decoder.hpp"
#undef private
#include "../src/feed/decoder.cpp"

#include <cassert>
#include <sys/socket.h>

static std::deque<int> ready;
static unsigned released = 0, submitted = 0;
static int64_t last_queued_pts = 0;
static uint32_t last_queued_flags = 0;
static bool input_blocked = false;
static bool invalid_last = false, queue_fails = false;
static size_t input_capacity = 4096;
static uint8_t output[6], input[4096];

extern "C" ssize_t __wrap_AMediaCodec_dequeueInputBuffer(AMediaCodec*, int64_t timeout)
{
    assert(timeout == 0); // live input never blocks output draining
    return !input_blocked && ready.empty() ? 0 : AMEDIACODEC_INFO_TRY_AGAIN_LATER;
}
extern "C" uint8_t* __wrap_AMediaCodec_getInputBuffer(AMediaCodec*, size_t, size_t* size)
{
    *size = input_capacity;
    return input;
}
extern "C" media_status_t __wrap_AMediaCodec_queueInputBuffer(
    AMediaCodec*, size_t, off_t, size_t, uint64_t pts, uint32_t flags)
{
    if (queue_fails) return AMEDIA_ERROR_UNKNOWN;
    last_queued_pts = pts;
    last_queued_flags = flags;
    ++submitted;
    ready.push_back(99);
    return AMEDIA_OK;
}
extern "C" ssize_t __wrap_AMediaCodec_dequeueOutputBuffer(
    AMediaCodec*, AMediaCodecBufferInfo* info, int64_t timeout)
{
    assert(timeout == 0);
    if (ready.empty()) return AMEDIACODEC_INFO_TRY_AGAIN_LATER;
    int value = ready.front();
    ready.pop_front();
    memset(output, value, sizeof(output));
    *info = {};
    info->size = invalid_last && ready.empty() ? 0 : sizeof(output);
    info->presentationTimeUs = value == 99 ? last_queued_pts : value;
    if (value == 99) info->flags = last_queued_flags;
    if (info->flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) info->size = 0;
    return value;
}
extern "C" uint8_t* __wrap_AMediaCodec_getOutputBuffer(AMediaCodec*, size_t, size_t* size)
{
    *size = sizeof(output);
    return output;
}
extern "C" media_status_t __wrap_AMediaCodec_releaseOutputBuffer(AMediaCodec*, size_t, bool)
{
    ++released;
    return AMEDIA_OK;
}

int main()
{
    recam::VideoDecoder decoder;
    auto* d = new recam::VideoDecoder::Impl();
    decoder.mImpl = d;
    d->codec = reinterpret_cast<AMediaCodec*>(1);
    d->started = d->formatSeen = d->formatReady = true;
    d->profiling = true;
    d->outStride = d->outSliceHeight = 2;
    d->outColor = 19;
    d->cropRight = d->cropBottom = 1;
    int pair[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, pair) == 0);
    d->sock = pair[0];

    ready = {1, 2, 3};
    d->pending[3].wireOffset = 300;
    decoder.drain_available();
    assert(released == 3 && ready.empty());
    recam::SrcFrame frame;
    assert(decoder.describe_frame(&frame));
    assert(frame.y.data[0] == 3 && frame.timing.wireOffset == 300);

    // A malformed later output must not replace the valid frame's timing.
    ready = {4, 5};
    invalid_last = true;
    d->pending[4].wireOffset = 400;
    decoder.drain_available();
    assert(decoder.describe_frame(&frame));
    assert(frame.y.data[0] == 4 && frame.timing.wireOffset == 400);
    assert(released == 5);
    invalid_last = false;

    // Input becomes available only after old outputs are released. The pump
    // must submit the pending AU without a wait, then expose the newest output.
    decoder.release();
    ready = {6, 7};
    d->au = {0, 0, 0, 1, 0x65, 0x80};
    d->buf = {0, 0, 0, 1, 0x65, 0x80, 0, 0, 0, 1, 0x65, 0x80};
    d->arrival.append(d->buf.size(), 1);
    assert(decoder.next(&frame));
    assert(submitted == 1 && ready.empty() && frame.y.data[0] == 99);
    assert(released == 8);
    // No new codec output means no new frame, not a duplicate publication.
    assert(!decoder.next(&frame));

    // A complete FLV tag queues immediately even with the socket still open and
    // no byte of the next picture available. TCP fragmentation must not submit it early.
    decoder.release();
    d->buf.clear(); d->au.clear(); d->arrival = {};
    d->transport = 2; d->flvHeader = true; d->nalLengthBytes = 4;
    std::vector<uint8_t> tag = {9,0,0,11,0,0,0,0,0,0,0,
                              0x17,1,0,0,0,0,0,0,2,0x65,0x80,0,0,0,22};
    const unsigned before = submitted;
    assert(write(pair[1], tag.data(), tag.size()-1) == (ssize_t)tag.size()-1);
    assert(!decoder.next(&frame) && submitted == before);
    assert(write(pair[1], tag.data()+tag.size()-1, 1) == 1);
    assert(decoder.next(&frame) && submitted == before+1);
    assert(frame.timing.wireOffset == 20 && frame.timing.receivedNs);
    assert(d->buf.empty() && d->au.empty());

    input_blocked = true;
    assert(write(pair[1], tag.data(), tag.size()) == (ssize_t)tag.size());
    assert(!decoder.next(&frame) && !d->au.empty() && d->buf.empty());
    assert(submitted == before+1);
    input_blocked = false;
    assert(decoder.next(&frame) && submitted == before+2 && d->au.empty());

    // Losing an encoded reference must terminate this connection, not silently
    // continue with dependent pictures after throwing the AU away.
    d->au = {1, 2, 3};
    input_capacity = 1;
    assert(!decoder.queue_au() && d->ended && !d->au.empty());
    ready.clear();
    d->ended = false;
    input_capacity = sizeof(input);
    queue_fails = true;
    assert(!decoder.queue_au() && d->ended && !d->au.empty());

    queue_fails = false;
    d->ended = false;
    d->au.clear();
    ready.clear();
    ::close(pair[1]);
    assert(!decoder.next(&frame) && d->readClosed && d->inputEos && d->ended);
    d->codec = nullptr; // fake codec; normal destructor still owns the socket
    puts("decoder scheduling checks passed");
}
