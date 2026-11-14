#include "decoder.hpp"
#include "h264.hpp"
#include "flv.hpp"

#include <media/NdkMediaCodec.h>
#include <media/NdkMediaExtractor.h>
#include <media/NdkMediaFormat.h>

#include <dlfcn.h>
#include <errno.h>
#include <time.h>
#include <vector>
#include <cstdlib>
#include <fcntl.h>
#include <poll.h>
#include <string.h>
#include <unistd.h>

#include <KittyUtils.hpp>

namespace recam {

namespace {

constexpr int32_t kColorYuv420Planar           = 19;
constexpr int32_t kColorYuv420PackedPlanar     = 20;
constexpr int32_t kColorYuv420SemiPlanar       = 21;
constexpr int32_t kColorYuv420PackedSemiPlanar = 39;

// libbinder_ndk exports these, but the NDK sysroot has no header for them.
void start_binder_threadpool_once()
{
    static bool done = false;
    if (done) return;
    done = true;

    void* lib = dlopen("libbinder_ndk.so", RTLD_NOW);
    if (!lib) {
        KITTY_LOGE("decoder: libbinder_ndk.so is not loadable; MediaCodec will stall.");
        return;
    }
    auto setMax = reinterpret_cast<bool (*)(uint32_t)>(
        dlsym(lib, "ABinderProcess_setThreadPoolMaxThreadCount"));
    auto start = reinterpret_cast<void (*)()>(dlsym(lib, "ABinderProcess_startThreadPool"));
    if (!start) {
        KITTY_LOGE("decoder: ABinderProcess_startThreadPool missing; MediaCodec will stall.");
        return;
    }
    if (setMax) setMax(4);
    start();
}

// A stalled decoder must not turn a socket into an unbounded allocation.
const size_t kMaxBacklog = 8u * 1024 * 1024;

uint64_t mono_ns()
{
    struct timespec ts {};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull + static_cast<uint64_t>(ts.tv_nsec);
}

}  // namespace

struct VideoDecoder::Impl {
    int              fd       = -1;
    AMediaExtractor* ex       = nullptr;
    AMediaCodec*     codec    = nullptr;
    bool             inputEos = false;

    std::vector<uint8_t> frame;
    bool                 frameReady = false;
    bool                 formatSeen = false;
    bool                 formatReady = false;
    int32_t              outStride = 0, outSliceHeight = 0;
    int32_t              outColor = 0;
    int32_t              cropLeft = 0, cropTop = 0, cropRight = 0, cropBottom = 0;
    bool                 warnedOutput = false;
    bool                 warnedLayout = false;

    // Stream mode. sock >= 0 means input arrives over the wire, not from `ex`.
    int                  sock  = -1;
    bool                 ended = false;
    bool                 readClosed = false;
    uint64_t             eofNs = 0;
    int                  transport = 0; // 0 undecided, 1 Annex-B, 2 FLV/AVC
    bool                 flvHeader = false;
    unsigned             nalLengthBytes = 0;
    std::vector<uint8_t> buf;              // undecoded Annex-B bytes
    std::vector<uint8_t> sps, pps;         // csd-0 and csd-1, once seen
    bool                 started = false;  // codec configured and running
    int64_t              ptsUs   = 0;
    std::vector<uint8_t> au;               // slices of the picture being assembled
    uint64_t qIn = 0, qOut = 0, qImg = 0, lastStatNs = 0;
    bool     warnedFit = false;
    bool     warnedBacklog = false;
};

VideoDecoder::~VideoDecoder() { close(); }

bool VideoDecoder::open(const char* path)
{
    close();

    // Without a binder threadpool the Codec2 callback never lands and input stalls.
    start_binder_threadpool_once();

    Impl* d = new Impl();
    mImpl = d;

    d->fd = ::open(path, O_RDONLY | O_CLOEXEC);
    if (d->fd < 0) {
        KITTY_LOGE("decoder: cannot open %s", path);
        close();
        return false;
    }
    const off_t len = lseek(d->fd, 0, SEEK_END);
    lseek(d->fd, 0, SEEK_SET);

    d->ex = AMediaExtractor_new();
    if (AMediaExtractor_setDataSourceFd(d->ex, d->fd, 0, len) != AMEDIA_OK) {
        KITTY_LOGE("decoder: %s is not a container MediaExtractor can read", path);
        close();
        return false;
    }

    int           track = -1;
    AMediaFormat* fmt   = nullptr;
    const char*   mime  = nullptr;
    const size_t  n     = AMediaExtractor_getTrackCount(d->ex);
    for (size_t i = 0; i < n; i++) {
        AMediaFormat* f = AMediaExtractor_getTrackFormat(d->ex, i);
        const char* m = nullptr;
        AMediaFormat_getString(f, AMEDIAFORMAT_KEY_MIME, &m);
        if (m && strncmp(m, "video/", 6) == 0 && track < 0) {
            track = static_cast<int>(i);
            mime  = m;
            fmt   = f;
            continue;
        }
        AMediaFormat_delete(f);
    }
    if (track < 0) {
        KITTY_LOGE("decoder: %s has no video track", path);
        close();
        return false;
    }

    AMediaFormat_getInt32(fmt, AMEDIAFORMAT_KEY_WIDTH,  &mWidth);
    AMediaFormat_getInt32(fmt, AMEDIAFORMAT_KEY_HEIGHT, &mHeight);
    int32_t fps = 0;
    if (AMediaFormat_getInt32(fmt, AMEDIAFORMAT_KEY_FRAME_RATE, &fps) && fps > 0)
        mIntervalUs = 1000000 / fps;

    if (mWidth < 2 || mHeight < 2) {
        KITTY_LOGE("decoder: implausible track size %dx%d", mWidth, mHeight);
        AMediaFormat_delete(fmt);
        close();
        return false;
    }

    AMediaExtractor_selectTrack(d->ex, track);

    d->codec = AMediaCodec_createDecoderByType(mime);
    if (!d->codec) {
        KITTY_LOGE("decoder: no decoder for %s", mime);
        AMediaFormat_delete(fmt);
        close();
        return false;
    }
    if (AMediaCodec_configure(d->codec, fmt, nullptr, nullptr, 0) != AMEDIA_OK ||
        AMediaCodec_start(d->codec) != AMEDIA_OK) {
        KITTY_LOGE("decoder: configure/start failed for %s", mime);
        AMediaFormat_delete(fmt);
        close();
        return false;
    }
    KITTY_LOGI("decoder: %s, %s %dx%d, %lld us/frame", path, mime, mWidth, mHeight,
               static_cast<long long>(mIntervalUs));
    AMediaFormat_delete(fmt); // mime points into this format
    return true;
}

void VideoDecoder::close()
{
    if (!mImpl) return;
    release();
    Impl* d = mImpl;
    mImpl = nullptr;

    if (d->codec) {
        AMediaCodec_stop(d->codec);
        AMediaCodec_delete(d->codec);
    }
    if (d->ex) AMediaExtractor_delete(d->ex);
    if (d->fd >= 0)   ::close(d->fd);
    if (d->sock >= 0) ::close(d->sock);
    delete d;

    mWidth = mHeight = 0;
}

bool VideoDecoder::open_stream(int fd)
{
    close();
    start_binder_threadpool_once();

    Impl* d = new Impl();
    mImpl = d;
    d->sock = fd;

    // Non-blocking: the feeder drains whatever has arrived and moves on.
    const int fl = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, (fl < 0 ? 0 : fl) | O_NONBLOCK);

    mIntervalUs = 33333;   // no VUI in most streams; the sender paces anyway
    KITTY_LOGI("decoder: reading H.264 (Annex-B or FLV), waiting for SPS/PPS");
    return true;
}

bool VideoDecoder::stream_ended() const
{
    return mImpl && mImpl->sock >= 0 && mImpl->ended;
}

// The SPS carries the geometry, so the codec can only start once one has arrived.
bool VideoDecoder::start_codec_from_csd()
{
    Impl* d = mImpl;
    if (d->sps.empty() || d->pps.empty()) return false;

    const h264::SpsInfo info = h264::parse_sps(d->sps.data(), d->sps.size());
    if (!info.ok) {
        KITTY_LOGE("decoder: the stream's SPS did not parse; dropping the connection");
        d->ended = true;
        return false;
    }
    mWidth  = info.width;
    mHeight = info.height;

    // csd-0 and csd-1 must carry their start codes; MediaCodec expects Annex-B.
    static const uint8_t kStart[4] = { 0, 0, 0, 1 };
    std::vector<uint8_t> csd0(kStart, kStart + 4), csd1(kStart, kStart + 4);
    csd0.insert(csd0.end(), d->sps.begin(), d->sps.end());
    csd1.insert(csd1.end(), d->pps.begin(), d->pps.end());

    AMediaFormat* fmt = AMediaFormat_new();
    AMediaFormat_setString(fmt, AMEDIAFORMAT_KEY_MIME, "video/avc");
    AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_WIDTH,  mWidth);
    AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_HEIGHT, mHeight);
    // Default input buffers are far too small for a 1080p IDR, which would be dropped.
    AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_MAX_INPUT_SIZE, mWidth * mHeight * 3 / 2);
    AMediaFormat_setBuffer(fmt, "csd-0", csd0.data(), csd0.size());
    AMediaFormat_setBuffer(fmt, "csd-1", csd1.data(), csd1.size());

    d->codec = AMediaCodec_createDecoderByType("video/avc");
    if (!d->codec ||
        AMediaCodec_configure(d->codec, fmt, nullptr, nullptr, 0) != AMEDIA_OK ||
        AMediaCodec_start(d->codec) != AMEDIA_OK) {
        KITTY_LOGE("decoder: could not start a decoder for the stream");
        AMediaFormat_delete(fmt);
        d->ended = true;
        return false;
    }
    AMediaFormat_delete(fmt);

    d->started = true;
    KITTY_LOGI("decoder: stream is video/avc %dx%d", mWidth, mHeight);
    return true;
}

// Hand the assembled picture to the codec as one input buffer.
bool VideoDecoder::queue_au()
{
    Impl* d = mImpl;
    if (d->au.empty()) return true;

    const ssize_t in = AMediaCodec_dequeueInputBuffer(d->codec, 0);
    if (in < 0) return false;

    size_t cap = 0;
    uint8_t* dst = AMediaCodec_getInputBuffer(d->codec, in, &cap);
    if (!dst || cap < d->au.size()) {
        // Dropping it silently would stall the decoder with no visible cause.
        if (!d->warnedFit) {
            d->warnedFit = true;
            KITTY_LOGE("decoder: a %zu-byte picture does not fit a %zu-byte input buffer",
                       d->au.size(), cap);
        }
        AMediaCodec_queueInputBuffer(d->codec, in, 0, 0, d->ptsUs, 0);
        d->ended = true; // a missing reference picture invalidates subsequent AUs
        return false;
    } else {
        memcpy(dst, d->au.data(), d->au.size());
        const auto rc = AMediaCodec_queueInputBuffer(d->codec, in, 0, d->au.size(), d->ptsUs, 0);
        if (rc != AMEDIA_OK) {
            KITTY_LOGE("decoder: queue input failed (%d); closing stream", rc);
            d->ended = true;
            return false;
        }
        d->qIn++;
    }

    d->ptsUs += mIntervalUs;
    d->au.clear();
    return true;
}

// Alternate nonblocking output drains with input submission to avoid backpressure.
bool VideoDecoder::pump_stream()
{
    Impl* d = mImpl;
    if (d->started) drain_available();

    uint8_t chunk[65536];
    for (int i = 0; i < 8 && !d->readClosed; i++) {
        const ssize_t n = ::read(d->sock, chunk, sizeof(chunk));
        if (n > 0) {
            d->buf.insert(d->buf.end(), chunk, chunk + n);
            continue;
        }
        if (n == 0) { d->readClosed = true; d->eofNs = mono_ns(); break; }
        if (errno == EINTR) continue;
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            KITTY_LOGW("decoder: stream read: %s", strerror(errno));
            d->ended = true;
        }
        break;
    }

    if (!d->transport && d->buf.size() >= 3) {
        if (!memcmp(d->buf.data(), "FLV", 3)) d->transport = 2;
        else if (d->buf[0] == 0) d->transport = 1;
        else { KITTY_LOGE("decoder: unsupported transport"); d->ended = true; }
    }
    if (!d->ended && d->transport == 2) pump_flv();
    else if (!d->ended && d->transport == 1) pump_annexb();
    if (d->ended) return d->frameReady;

    if (d->readClosed) {
        if (!d->started) { d->ended = true; return false; }
        if (d->buf.empty() && d->au.empty() && !d->inputEos) {
            const ssize_t slot = AMediaCodec_dequeueInputBuffer(d->codec, 0);
            if (slot >= 0) {
                if (AMediaCodec_queueInputBuffer(d->codec, slot, 0, 0, d->ptsUs,
                        AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) != AMEDIA_OK) d->ended = true;
                else d->inputEos = true;
            }
        }
        if (mono_ns() - d->eofNs > 2000000000ull) {
            KITTY_LOGW("decoder: output EOS timed out; closing stream");
            d->ended = true;
        }
    }

    // Do not discard reference pictures and then continue a dependent stream.
    if (d->buf.size() + d->au.size() > kMaxBacklog) {
        if (!d->warnedBacklog) {
            d->warnedBacklog = true;
            KITTY_LOGW("decoder: %zu bytes backed up; closing stream for a fresh connection", d->buf.size() + d->au.size());
        }
        d->ended = true;
        return d->frameReady;
    }

    if (!d->started) return false;

    drain_available();

    const uint64_t nowNs = mono_ns();
    if (nowNs - d->lastStatNs > 5ull * 1000 * 1000 * 1000) {
        d->lastStatNs = nowNs;
        KITTY_LOGI("decoder: stream pictures in=%llu out=%llu shown=%llu, %zu bytes queued",
                   static_cast<unsigned long long>(d->qIn),
                   static_cast<unsigned long long>(d->qOut),
                   static_cast<unsigned long long>(d->qImg), d->buf.size());
    }
    return d->frameReady;
}

void VideoDecoder::pump_annexb()
{
    Impl* d = mImpl;
    // Keep the tail: a start code may be split across two reads.
    size_t pos = 0, consumed = 0;
    h264::Nal nal;
    while (h264::next_nal(d->buf.data(), d->buf.size(), &pos, &nal)) {
        if (pos >= d->buf.size() && !d->readClosed) break;   // last NAL may be incomplete

        if (nal.type == h264::kNalSps && d->sps.empty())
            d->sps.assign(nal.data, nal.data + nal.size);
        else if (nal.type == h264::kNalPps && d->pps.empty())
            d->pps.assign(nal.data, nal.data + nal.size);

        if (!d->started && !start_codec_from_csd()) { consumed = pos; continue; }

        // Sliced-thread encoders split one picture across several slice NALs.
        if (h264::is_vcl(nal.type) && h264::starts_picture(nal.data, nal.size) &&
            !d->au.empty()) {
            // Output buffers must be released before asking for more input slots.
            drain_available();
            if (!queue_au()) break;   // keep the complete AU and retry next poll
            drain_available();
        }

        // Parameter sets already went in as csd; re-sending them is harmless noise.
        if (nal.type != h264::kNalSps && nal.type != h264::kNalPps) {
            static const uint8_t kStart[4] = { 0, 0, 0, 1 };
            d->au.insert(d->au.end(), kStart, kStart + 4);
            d->au.insert(d->au.end(), nal.data, nal.data + nal.size);
        }

        consumed = pos;
    }

    if (consumed) {
        d->buf.erase(d->buf.begin(), d->buf.begin() + consumed);
    }

    if (d->readClosed && d->buf.empty() && d->started && !d->au.empty()) {
        drain_available();
        queue_au();
    }
}

void VideoDecoder::pump_flv()
{
    Impl* d = mImpl;
    auto invalid = [&] {
        KITTY_LOGE("decoder: invalid or unsupported FLV/AVC stream; closing connection");
        d->ended = true;
    };
    auto discard = [&](size_t n) {
        d->buf.erase(d->buf.begin(), d->buf.begin() + n);
    };
    if (!d->au.empty()) {
        drain_available();
        if (!queue_au()) return; // retain the whole picture until input is available
    }
    if (!d->flvHeader) {
        size_t n = 0;
        const auto r = flv::header(d->buf.data(), d->buf.size(), n);
        if (r != flv::Result::Ready) {
            if (r == flv::Result::Invalid || d->readClosed) invalid();
            return;
        }
        discard(n);
        d->flvHeader = true;
    }
    while (!d->buf.empty() && !d->ended) {
        flv::Tag tag{};
        const auto r = flv::tag(d->buf.data(), d->buf.size(), tag);
        if (r != flv::Result::Ready) {
            if (r == flv::Result::Invalid || d->readClosed) invalid();
            return;
        }
        if (tag.type == 9) {
            const auto* p = tag.data;
            if (tag.size < 5 || (p[0] & 15) != 7 || (p[0] >> 4) < 1 ||
                (p[0] >> 4) > 2 || flv::be(p + 2, 3) != 0) { invalid(); return; }
            if (p[1] == 0) {
                std::vector<uint8_t> sps, pps;
                unsigned width = 0;
                if (!flv::config(p + 5, tag.size - 5, width, sps, pps) ||
                    (d->started && (sps != d->sps || pps != d->pps))) { invalid(); return; }
                d->sps = std::move(sps); d->pps = std::move(pps);
                d->nalLengthBytes = width;
                if (!d->started && !start_codec_from_csd()) { invalid(); return; }
            } else if (p[1] == 1) {
                if (!d->started || !flv::picture(p + 5, tag.size - 5, d->nalLengthBytes,
                                                 d->au)) { invalid(); return; }
            } else if (p[1] != 2 || tag.size != 5) { invalid(); return; }
        }
        discard(tag.bytes);
        if (!d->au.empty()) {
            drain_available();
            if (!queue_au()) return;
            drain_available();
        }
    }
}

void VideoDecoder::rewind()
{
    Impl* d = mImpl;
    AMediaExtractor_seekTo(d->ex, 0, AMEDIAEXTRACTOR_SEEK_PREVIOUS_SYNC);
    AMediaCodec_flush(d->codec);
    d->inputEos = false;
}

// Feed one input buffer and copy one decoded output frame.
bool VideoDecoder::pump()
{
    Impl* d = mImpl;
    if (d->sock >= 0) return pump_stream();

    if (!d->inputEos) {
        const ssize_t in = AMediaCodec_dequeueInputBuffer(d->codec, 2000);
        if (in >= 0) {
            size_t cap = 0;
            uint8_t* buf = AMediaCodec_getInputBuffer(d->codec, in, &cap);
            const ssize_t got = buf ? AMediaExtractor_readSampleData(d->ex, buf, cap) : -1;
            if (got < 0) {
                // Loop the file rather than signalling EOS, which would need a restart.
                rewind();
                AMediaCodec_queueInputBuffer(d->codec, in, 0, 0, 0, 0);
            } else {
                AMediaCodec_queueInputBuffer(d->codec, in, 0, static_cast<size_t>(got),
                                             AMediaExtractor_getSampleTime(d->ex), 0);
                AMediaExtractor_advance(d->ex);
            }
        }
    }

    return drain_output("file");
}

bool VideoDecoder::update_output_format(const char* source)
{
    Impl* d = mImpl;
    AMediaFormat* fmt = AMediaCodec_getOutputFormat(d->codec);
    if (!fmt) return false;

    int32_t width = mWidth, height = mHeight, stride = 0, sliceHeight = 0, color = 0;
    AMediaFormat_getInt32(fmt, AMEDIAFORMAT_KEY_WIDTH, &width);
    AMediaFormat_getInt32(fmt, AMEDIAFORMAT_KEY_HEIGHT, &height);
    AMediaFormat_getInt32(fmt, AMEDIAFORMAT_KEY_STRIDE, &stride);
    AMediaFormat_getInt32(fmt, AMEDIAFORMAT_KEY_SLICE_HEIGHT, &sliceHeight);
    const bool haveColor = AMediaFormat_getInt32(fmt, AMEDIAFORMAT_KEY_COLOR_FORMAT, &color);

    if (stride <= 0) stride = width;
    if (sliceHeight <= 0) sliceHeight = height;

    int32_t left = 0, top = 0, right = width - 1, bottom = height - 1;
    int32_t cropLeft, cropTop, cropRight, cropBottom;
    if (AMediaFormat_getInt32(fmt, "crop-left", &cropLeft) &&
        AMediaFormat_getInt32(fmt, "crop-top", &cropTop) &&
        AMediaFormat_getInt32(fmt, "crop-right", &cropRight) &&
        AMediaFormat_getInt32(fmt, "crop-bottom", &cropBottom)) {
        left = cropLeft;
        top = cropTop;
        right = cropRight;
        bottom = cropBottom;
    } else {
        AMediaFormat_getRect(fmt, AMEDIAFORMAT_KEY_DISPLAY_CROP,
                             &left, &top, &right, &bottom);
    }

    d->outStride = stride;
    d->outSliceHeight = sliceHeight;
    d->outColor = color;
    d->cropLeft = left;
    d->cropTop = top;
    d->cropRight = right;
    d->cropBottom = bottom;

    const int32_t visibleWidth = right - left + 1;
    const int32_t visibleHeight = bottom - top + 1;
    d->formatSeen = true;
    d->formatReady = haveColor && width >= 2 && height >= 2 && stride >= width &&
                     sliceHeight >= height && stride % 2 == 0 &&
                     sliceHeight % 2 == 0 && left >= 0 && top >= 0 &&
                     right >= left && bottom >= top && right < stride &&
                     bottom < sliceHeight && visibleWidth % 2 == 0 &&
                     visibleHeight % 2 == 0 && left % 2 == 0 && top % 2 == 0;

    KITTY_LOGI("decoder: %s output color=%d coded=%dx%d stride=%d slice=%d crop=%d,%d-%d,%d",
               source, color, width, height, stride, sliceHeight,
               left, top, right, bottom);
    if (!d->formatReady)
        KITTY_LOGE("decoder: %s output layout is incomplete or invalid (%s)",
                   source, AMediaFormat_toString(fmt));

    AMediaFormat_delete(fmt);
    return d->formatReady;
}

bool VideoDecoder::drain_output(const char* source, int64_t timeoutUs, bool* progressed)
{
    Impl* d = mImpl;
    AMediaCodecBufferInfo info{};
    if (progressed) *progressed = false;
    const ssize_t out = AMediaCodec_dequeueOutputBuffer(d->codec, &info, timeoutUs);
    if (out == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
        if (progressed) *progressed = true;
        d->frameReady = false; // an older frame has the old plane geometry
        update_output_format(source);
        return false;
    }
    if (out < 0) return false;
    if (progressed) *progressed = true;
    if (d->sock >= 0 && (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM)) d->ended = true;
    if (!d->formatSeen) update_output_format(source);

    size_t bytes = 0;
    uint8_t* data = AMediaCodec_getOutputBuffer(d->codec, static_cast<size_t>(out), &bytes);
    const size_t offset = info.offset >= 0 ? static_cast<size_t>(info.offset) : bytes;
    const size_t size = info.size > 0 ? static_cast<size_t>(info.size) : 0;
    const bool validRange = data && offset <= bytes && size > 0 && size <= bytes - offset;
    bool copied = false;
    if (validRange) {
        d->frame.assign(data + offset, data + offset + size);
        d->frameReady = true;
        copied = true;
    } else if (!d->warnedOutput && !(info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM)) {
        d->warnedOutput = true;
        KITTY_LOGE("decoder: %s returned an invalid output range (%d+%d of %zu)",
                   source, info.offset, info.size, bytes);
    }

    AMediaCodec_releaseOutputBuffer(d->codec, static_cast<size_t>(out), false);
    d->qOut++;
    return copied;
}

void VideoDecoder::drain_available()
{
    // Bound work under an unusually deep output queue. Only the newest copied
    // output is exposed to the caller; every codec buffer is released promptly.
    for (int i = 0; i < 64; ++i) {
        bool progressed = false;
        drain_output("stream", 0, &progressed);
        if (!progressed) break;
    }
}

void VideoDecoder::wait_stream(int timeoutMs)
{
    if (!mImpl || mImpl->sock < 0) return;
    if (mImpl->readClosed) { usleep(static_cast<useconds_t>(timeoutMs * 1000)); return; }
    pollfd fd{mImpl->sock, POLLIN, 0};
    // MediaCodec's synchronous API has no pollable output fd. Recheck it within
    // timeoutMs even when the sender has no more bytes ready.
    poll(&fd, 1, timeoutMs);
}

bool VideoDecoder::describe_frame(SrcFrame* out)
{
    Impl* d = mImpl;
    if (!d->frameReady || !d->formatReady) return false;

    const int32_t width = d->cropRight - d->cropLeft + 1;
    const int32_t height = d->cropBottom - d->cropTop + 1;
    const int32_t stride = d->outStride;
    const int32_t sliceHeight = d->outSliceHeight;
    const size_t yBytes = static_cast<size_t>(stride) * sliceHeight;
    size_t required = 0;
    size_t cbOffset = 0, crOffset = 0;
    int32_t chromaStride = 0, chromaPixelStride = 0;

    if (d->outColor == kColorYuv420Planar ||
        d->outColor == kColorYuv420PackedPlanar) {
        chromaStride = stride / 2;
        chromaPixelStride = 1;
        const size_t chromaBytes = static_cast<size_t>(chromaStride) * (sliceHeight / 2);
        required = yBytes + chromaBytes * 2;
        const size_t cropOffset = static_cast<size_t>(d->cropTop / 2) * chromaStride +
                                  d->cropLeft / 2;
        cbOffset = yBytes + cropOffset;
        crOffset = yBytes + chromaBytes + cropOffset;
    } else if (d->outColor == kColorYuv420SemiPlanar ||
               d->outColor == kColorYuv420PackedSemiPlanar) {
        chromaStride = stride;
        chromaPixelStride = 2;
        required = yBytes + static_cast<size_t>(stride) * (sliceHeight / 2);
        const size_t cropOffset = static_cast<size_t>(d->cropTop / 2) * stride +
                                  d->cropLeft;
        cbOffset = yBytes + cropOffset;
        crOffset = cbOffset + 1;
    } else {
        if (!d->warnedLayout) {
            d->warnedLayout = true;
            KITTY_LOGE("decoder: unsupported bytebuffer color format %d", d->outColor);
        }
        return false;
    }

    if (d->frame.size() < required) {
        if (!d->warnedLayout) {
            d->warnedLayout = true;
            KITTY_LOGE("decoder: output frame is %zu bytes, layout needs %zu",
                       d->frame.size(), required);
        }
        return false;
    }

    const uint8_t* base = d->frame.data();
    out->width = width;
    out->height = height;
    out->y = { base + static_cast<size_t>(d->cropTop) * stride + d->cropLeft,
               stride, 1 };
    out->cb = { base + cbOffset, chromaStride, chromaPixelStride };
    out->cr = { base + crOffset, chromaStride, chromaPixelStride };

    d->qImg++;
    return true;
}

bool VideoDecoder::next(SrcFrame* out)
{
    if (!mImpl || !out) return false;
    Impl* d = mImpl;
    release();

    if (d->sock >= 0) {
        pump_stream();
    } else {
        // File playback retains the existing pacing and one-frame semantics.
        for (int i = 0; i < 64 && !d->frameReady; i++) pump();
    }
    return describe_frame(out);
}

void VideoDecoder::release()
{
    if (mImpl) mImpl->frameReady = false;
}

}  // namespace recam
