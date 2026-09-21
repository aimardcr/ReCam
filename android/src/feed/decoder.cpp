#include "decoder.hpp"
#include "h264.hpp"

#include <media/NdkImage.h>
#include <media/NdkImageReader.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaExtractor.h>
#include <media/NdkMediaFormat.h>

#include <dlfcn.h>
#include <errno.h>
#include <time.h>
#include <vector>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

#include <KittyUtils.hpp>

namespace recam {

namespace {

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
    AImageReader*    reader   = nullptr;
    ANativeWindow*   window   = nullptr;
    bool             inputEos = false;
    AImage*          held     = nullptr;   // planes handed out by next()

    // Stream mode. sock >= 0 means input arrives over the wire, not from `ex`.
    int                  sock  = -1;
    bool                 ended = false;
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

    // maxImages 4 matches the ring, so the decoder is never starved by the consumer.
    if (AImageReader_new(mWidth, mHeight, AIMAGE_FORMAT_YUV_420_888, 4, &d->reader) != AMEDIA_OK ||
        AImageReader_getWindow(d->reader, &d->window) != AMEDIA_OK || !d->window) {
        KITTY_LOGE("decoder: could not create the AImageReader surface");
        AMediaFormat_delete(fmt);
        close();
        return false;
    }

    d->codec = AMediaCodec_createDecoderByType(mime);
    if (!d->codec) {
        KITTY_LOGE("decoder: no decoder for %s", mime);
        AMediaFormat_delete(fmt);
        close();
        return false;
    }
    if (AMediaCodec_configure(d->codec, fmt, d->window, nullptr, 0) != AMEDIA_OK ||
        AMediaCodec_start(d->codec) != AMEDIA_OK) {
        KITTY_LOGE("decoder: configure/start failed for %s", mime);
        AMediaFormat_delete(fmt);
        close();
        return false;
    }
    AMediaFormat_delete(fmt);

    KITTY_LOGI("decoder: %s, %s %dx%d, %lld us/frame", path, mime, mWidth, mHeight,
               static_cast<long long>(mIntervalUs));
    return true;
}

void VideoDecoder::close()
{
    if (!mImpl) return;
    release();
    Impl* d = mImpl;
    mImpl = nullptr;   // the reader's looper must not see a half-torn-down decoder

    if (d->codec) {
        AMediaCodec_stop(d->codec);
        AMediaCodec_delete(d->codec);
    }
    if (d->reader) AImageReader_delete(d->reader);   // also releases d->window
    if (d->ex)      AMediaExtractor_delete(d->ex);
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
    KITTY_LOGI("decoder: reading an Annex-B H.264 stream, waiting for SPS/PPS");
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

    if (AImageReader_new(mWidth, mHeight, AIMAGE_FORMAT_YUV_420_888, 4, &d->reader) != AMEDIA_OK ||
        AImageReader_getWindow(d->reader, &d->window) != AMEDIA_OK || !d->window) {
        KITTY_LOGE("decoder: could not create the AImageReader surface");
        d->ended = true;
        return false;
    }

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
        AMediaCodec_configure(d->codec, fmt, d->window, nullptr, 0) != AMEDIA_OK ||
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

    const ssize_t in = AMediaCodec_dequeueInputBuffer(d->codec, 2000);
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
    } else {
        memcpy(dst, d->au.data(), d->au.size());
        AMediaCodec_queueInputBuffer(d->codec, in, 0, d->au.size(), d->ptsUs, 0);
        d->qIn++;
    }

    d->ptsUs += mIntervalUs;
    d->au.clear();
    return true;
}

// Drain the socket, then hand whole pictures to the codec.
bool VideoDecoder::pump_stream()
{
    Impl* d = mImpl;

    uint8_t chunk[65536];
    for (int i = 0; i < 8; i++) {
        const ssize_t n = ::read(d->sock, chunk, sizeof(chunk));
        if (n > 0) {
            d->buf.insert(d->buf.end(), chunk, chunk + n);
            continue;
        }
        if (n == 0) { d->ended = true; break; }
        if (errno == EINTR) continue;
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            KITTY_LOGW("decoder: stream read: %s", strerror(errno));
            d->ended = true;
        }
        break;
    }

    // Keep the tail: a start code may be split across two reads.
    size_t pos = 0, consumed = 0;
    h264::Nal nal;
    while (h264::next_nal(d->buf.data(), d->buf.size(), &pos, &nal)) {
        if (pos >= d->buf.size() && !d->ended) break;   // last NAL may be incomplete

        if (nal.type == h264::kNalSps && d->sps.empty())
            d->sps.assign(nal.data, nal.data + nal.size);
        else if (nal.type == h264::kNalPps && d->pps.empty())
            d->pps.assign(nal.data, nal.data + nal.size);

        if (!d->started && !start_codec_from_csd()) { consumed = pos; continue; }

        // Sliced-thread encoders split one picture across several slice NALs.
        if (h264::is_vcl(nal.type) && h264::starts_picture(nal.data, nal.size) &&
            !d->au.empty()) {
            if (!queue_au()) break;   // no input slot; the NAL stays for the next tick
        }

        // Parameter sets already went in as csd; re-sending them is harmless noise.
        if (nal.type != h264::kNalSps && nal.type != h264::kNalPps) {
            static const uint8_t kStart[4] = { 0, 0, 0, 1 };
            d->au.insert(d->au.end(), kStart, kStart + 4);
            d->au.insert(d->au.end(), nal.data, nal.data + nal.size);
        }

        consumed = pos;
    }

    if (consumed) d->buf.erase(d->buf.begin(), d->buf.begin() + consumed);

    // Nothing is draining, so drop the backlog and resync on the next IDR.
    if (d->buf.size() > kMaxBacklog) {
        if (!d->warnedBacklog) {
            d->warnedBacklog = true;
            KITTY_LOGW("decoder: %zu bytes backed up; dropping to resync", d->buf.size());
        }
        d->buf.clear();
        d->au.clear();
    }

    if (!d->started) return false;

    AMediaCodecBufferInfo info{};
    const ssize_t out = AMediaCodec_dequeueOutputBuffer(d->codec, &info, 2000);
    if (out >= 0) {
        AMediaCodec_releaseOutputBuffer(d->codec, out, true);
        d->qOut++;
    } else if (out == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
        AMediaFormat* of = AMediaCodec_getOutputFormat(d->codec);
        KITTY_LOGI("decoder: stream output format %s", AMediaFormat_toString(of));
        AMediaFormat_delete(of);
    }

    const uint64_t nowNs = mono_ns();
    if (nowNs - d->lastStatNs > 5ull * 1000 * 1000 * 1000) {
        d->lastStatNs = nowNs;
        KITTY_LOGI("decoder: stream pictures in=%llu out=%llu shown=%llu, %zu bytes queued",
                   static_cast<unsigned long long>(d->qIn),
                   static_cast<unsigned long long>(d->qOut),
                   static_cast<unsigned long long>(d->qImg), d->buf.size());
    }
    return out >= 0;
}

void VideoDecoder::rewind()
{
    Impl* d = mImpl;
    AMediaExtractor_seekTo(d->ex, 0, AMEDIAEXTRACTOR_SEEK_PREVIOUS_SYNC);
    AMediaCodec_flush(d->codec);
    d->inputEos = false;
}

// Feed one input buffer and drain one output buffer, rendering it into the reader.
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

    AMediaCodecBufferInfo info{};
    const ssize_t out = AMediaCodec_dequeueOutputBuffer(d->codec, &info, 2000);
    if (out >= 0) {
        AMediaCodec_releaseOutputBuffer(d->codec, out, true);
        return true;
    }
    return false;
}

bool VideoDecoder::next(SrcFrame* out)
{
    if (!mImpl || !out) return false;
    Impl* d = mImpl;
    release();

    const int spins = (d->sock >= 0) ? 4 : 64;   // a live stream must not block the tick
    for (int i = 0; i < spins && !d->held; i++) {
        pump();
        if (!d->reader) break;
        if (AImageReader_acquireLatestImage(d->reader, &d->held) != AMEDIA_OK) d->held = nullptr;
        if (d->held) d->qImg++;
    }
    if (!d->held) return false;

    int32_t planes = 0;
    AImage_getNumberOfPlanes(d->held, &planes);
    AImage_getWidth(d->held, &out->width);
    AImage_getHeight(d->held, &out->height);
    if (planes != 3 || out->width < 2 || out->height < 2) {
        release();
        return false;
    }

    SrcPlane* p[3] = { &out->y, &out->cb, &out->cr };
    for (int i = 0; i < 3; i++) {
        uint8_t* data = nullptr;
        int len = 0;
        if (AImage_getPlaneData(d->held, i, &data, &len) != AMEDIA_OK || !data ||
            AImage_getPlaneRowStride(d->held, i, &p[i]->rowStride) != AMEDIA_OK ||
            AImage_getPlanePixelStride(d->held, i, &p[i]->pixelStride) != AMEDIA_OK) {
            release();
            return false;
        }
        p[i]->data = data;
    }
    return true;
}

void VideoDecoder::release()
{
    if (mImpl && mImpl->held) {
        AImage_delete(mImpl->held);
        mImpl->held = nullptr;
    }
}

}  // namespace recam
