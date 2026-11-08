// Stall readers across many publications; verify complete, immutable frames.
#include "../src/common/ring_access.hpp"
#include <array>
#include <atomic>
#include <cassert>
#include <cstdio>
#include <thread>
#include <vector>

struct Fixture {
    recam::ShmLane lane{};
    std::array<std::array<uint64_t, 1024>, recam::kLaneBufs> pixels{};
    bool publish() {
        recam::RingPublication p(lane);
        if (!p.seq) return false;
        pixels[p.slot].fill(p.seq);
        p.publish(p.seq * 1000);
        return true;
    }
    void verify(const recam::RingFrame& f) {
        assert(f.seq);
        for (auto pixel : pixels[f.slot]) assert(pixel == f.seq);
        assert(lane.slotNs[f.slot] == f.seq * 1000);
    }
};
int main() {
    Fixture f;
    assert(f.publish());
    {
        recam::RingRead access(&f.lane.access);
        recam::RingFrame first(f.lane);
        for (unsigned i = 0; i < 10000; ++i) assert(f.publish());
        f.verify(first); // the original implementation tore this frame
        recam::RingFrame second(f.lane);
        assert(f.publish());
        recam::RingFrame third(f.lane);
        assert(!f.publish()); // all slots held: keep the current complete frame
        f.verify(first); f.verify(second); f.verify(third);
        recam::RingWrite retire(&f.lane.access);
        assert(!retire);
    }
    assert(f.publish());
    {
        recam::RingWrite retire(&f.lane.access);
        assert(retire);
        recam::RingRead reader(&f.lane.access);
        assert(!reader);
    }
    {
        const uint64_t latest = f.lane.published;
        uint32_t abortedSlot;
        {
            recam::RingPublication aborted(f.lane);
            assert(aborted.seq);
            abortedSlot = aborted.slot;
            f.pixels[abortedSlot][0] = 0xdead;
        }
        assert(f.lane.slotSeq[abortedSlot] == 0 && f.lane.slots[abortedSlot] == 0);
        recam::RingFrame current(f.lane);
        assert(current.seq == latest);
        f.verify(current);
    }
    std::atomic<bool> done{false};
    std::atomic<unsigned> checked{0};
    std::vector<std::thread> readers;
    for (unsigned i = 0; i < 4; ++i) readers.emplace_back([&] {
        do {
            recam::RingRead access(&f.lane.access);
            assert(access);
            recam::RingFrame frame(f.lane);
            if (frame.seq) { std::this_thread::yield(); f.verify(frame); ++checked; }
        } while (!done.load());
    });
    for (unsigned i = 0; i < 100000; ++i) f.publish();
    done = true;
    for (auto& thread : readers) thread.join();
    assert(checked > 0);
    for (auto lock : f.lane.slots) assert(lock == 0);
    assert(f.lane.access == 0);
    printf("pinned-frame, saturation, retirement, and concurrent-copy checks passed (%u reads)\n", checked.load());
}
