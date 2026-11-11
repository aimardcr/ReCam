// Exercise the production consumer, bounded geometry, and fallback concurrently.
#include "../src/payload/shmsource.cpp"
#include "../src/payload/colorbars.cpp"
#include <cassert>
#include <cstdio>
#include <random>
#include <thread>
#include <vector>

using namespace recam;

struct Target {
    FrameTarget t;
    std::vector<uint8_t> y, uv, v;
    // 0 = planar, 1 = NV12, 2 = NV21, 3 = separated strided chroma.
    Target(int w, int h, int layout) {
        t.width = w; t.height = h; t.format = 0x23;
        const int ys = w + 32, ps = layout == 0 ? 1 : layout == 3 ? 4 : 2;
        const int cs = (w / 2) * ps + 32;
        y.assign(static_cast<size_t>(ys) * h + 64, 0xA5);
        uv.assign(static_cast<size_t>(cs) * (h / 2) + 64, 0xA5);
        v = uv;
        t.y = {y.data() + 32, ys, 1};
        t.cb = {uv.data() + 32, cs, ps};
        t.cr = {(layout == 1 || layout == 2 ? uv.data() + 33 : v.data() + 32), cs, ps};
        if (layout == 2) std::swap(t.cb.data, t.cr.data);
        assert(t.valid());
    }
    void check_padding() const {
        for (const auto* p : {&y, &uv, &v}) {
            for (size_t i = 0; i < 32; ++i) assert((*p)[i] == 0xA5);
            for (size_t i = p->size() - 32; i < p->size(); ++i) assert((*p)[i] == 0xA5);
        }
        for (int row = 0; row < t.height; ++row)
            for (int x = t.width; x < t.y.rowStride; ++x)
                assert(t.y.data[row * t.y.rowStride + x] == 0xA5);
    }
};

static uint8_t sample(const uint8_t* src, unsigned sw, unsigned sh,
                      unsigned dw, unsigned dh, unsigned x, unsigned y) {
    const unsigned sx = x * (dw > 1 ? ((sw - 1) << 16) / (dw - 1) : 0);
    const unsigned sy = y * (dh > 1 ? ((sh - 1) << 16) / (dh - 1) : 0);
    const unsigned x0 = sx >> 16, x1 = std::min(x0 + 1, sw - 1);
    const unsigned y0 = sy >> 16, y1 = std::min(y0 + 1, sh - 1);
    const auto row = [&](unsigned yy) {
        int a = src[yy * sw + x0], b = src[yy * sw + x1];
        return a + ((b - a) * static_cast<int>(sx & 65535) >> 16);
    };
    const int a = row(y0), b = row(y1);
    return a + ((b - a) * static_cast<int>(sy & 65535) >> 16);
}

static void differential(unsigned sw, unsigned sh, unsigned dw, unsigned dh, int layout) {
    std::mt19937 rng(sw + 13 * dh + layout);
    std::vector<uint8_t> src(sw * sh * 3 / 2);
    for (auto& x : src) x = rng();
    Target target(dw, dh, layout);
    assert(blit_i420(src.data(), sw, sh, target.t));
    const uint8_t* in = src.data();
    unsigned w = sw, h = sh, ow = dw, oh = dh;
    int planeIndex = 0;
    for (auto plane : {target.t.y, target.t.cb, target.t.cr}) {
        for (unsigned y = 0; y < oh; ++y)
            for (unsigned x = 0; x < ow; ++x)
                assert(plane.data[y * plane.rowStride + x * plane.pixelStride] ==
                       sample(in, w, h, ow, oh, x, y));
        in += w * h;
        if (planeIndex++ == 0) { w /= 2; h /= 2; ow /= 2; oh /= 2; }
    }
    target.check_padding();
}

static void large_ring(unsigned w, unsigned h) {
    Target target(w, h, 2);
    assert(!g_source.fill(target.t)); // requests a lane, not yet published
    ShmLane& lane = header()->lane[0];
    assert(lane.reqWidth == w && lane.reqHeight == h);
    uint32_t storedW, storedH;
    assert(lane_geometry(w, h, storedW, storedH));
    lane.width = storedW; lane.height = storedH;
    lane.bufBytes = lane_frame_bytes(storedW, storedH);
    lane.dataOffset = kShmArenaBase;
    assert(lane_configured(lane));
    assert(lane.bufBytes * kLaneBufs <= lane_slice_bytes());
    {
        RingPublication publication(lane);
        assert(publication.seq);
        auto* pixels = g_base + lane.dataOffset + publication.slot * lane.bufBytes;
        memset(pixels, 77, storedW * storedH);
        memset(pixels + storedW * storedH, 91, storedW * storedH / 4);
        memset(pixels + storedW * storedH * 5 / 4, 163, storedW * storedH / 4);
        publication.publish(now_ns());
    }
    assert(g_source.fill(target.t));
    int value = 77;
    unsigned pw = w, ph = h;
    for (auto plane : {target.t.y, target.t.cb, target.t.cr}) {
        for (unsigned y = 0; y < ph; ++y)
            for (unsigned x = 0; x < pw; ++x)
                assert(plane.data[y * plane.rowStride + x * plane.pixelStride] == value);
        if (value == 77) { pw /= 2; ph /= 2; value = 91; } else value = 163;
    }
    target.check_padding();
    assert(strcmp(lane_state(lane, true, now_ns()), "served") == 0);
    assert(strcmp(lane_state(lane, false, now_ns()), "feeder_stopped") == 0);
    assert(strcmp(lane_state(lane, true, lane.publishedNs + kFrameStaleNs + 1), "stale_frame") == 0);
    lane.slotNs[(lane.published - 1) % kLaneBufs] = 1;
    assert(!g_source.fill(target.t));
    memset(header(), 0, kShmArenaBase);
}

int main() {
    assert(shm_total_bytes() == 37605376); // memory budget must not grow
    std::vector<uint8_t> ring(shm_total_bytes());
    g_base = ring.data(); g_size = ring.size();
    for (auto size : {std::pair<unsigned,unsigned>{1960,882}, {4608,2080}, {4608,3456},
                      {4000,3000}, {4096,1840}, {1080,1920}, {8192,4096}, {1920,1080}})
        large_ring(size.first, size.second);
    g_base = nullptr; g_size = 0;

    std::mt19937 rng(2323);
    for (int i = 0; i < 120; ++i) {
        unsigned w = 2 + 2 * (rng() % 90), h = 2 + 2 * (rng() % 90);
        differential(w, h, i % 5 ? 2 + 2 * (rng() % 100) : w,
                     i % 5 ? 2 + 2 * (rng() % 100) : h, i % 4);
    }
    differential(1920,864,1960,882,2);
    differential(1440,1080,4608,3456,1);
    Target bad(16, 12, 1);
    auto before = bad.y;
    bad.t.cb.rowStride = 2;
    uint8_t src[6]{};
    assert(!blit_i420(src, 2, 2, bad.t) && bad.y == before);
    uint32_t w, h;
    for (auto dims : {std::pair<unsigned,unsigned>{0,2}, {3,2}, {UINT32_MAX,4},
                      {8192,8192}, {8194,2}})
        assert(!lane_geometry(dims.first, dims.second, w, h));

    // Simultaneous streams must never overwrite each other's fallback row cache.
    std::vector<std::thread> threads;
    for (int i = 0; i < 6; ++i) threads.emplace_back([i] {
        Target t(320 + i * 32, 240, i % 3);
        for (int n = 0; n < 100; ++n) {
            assert(colorbars_source()->fill(t.t));
            for (int x = 0; x < t.t.width; ++x)
                assert(t.t.y.data[x] == kBars[x * kBarCount / t.t.width].y);
            t.check_padding();
        }
    });
    for (auto& t : threads) t.join();
    puts("Large ring outputs, bilinear differential/canary checks, invalid layouts, and concurrent fallback passed");
}
