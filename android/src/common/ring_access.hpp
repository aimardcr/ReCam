// Nonblocking shared-memory ownership. One producer, multiple camera readers.
#pragma once
#include "shmframe.hpp"

namespace recam {
constexpr uint32_t kWriter = 0x80000000u;

inline bool try_read(uint32_t* word) {
    uint32_t value = __atomic_load_n(word, __ATOMIC_ACQUIRE);
    for (unsigned i = 0; i < 8 && value < kWriter - 1; ++i)
        if (__atomic_compare_exchange_n(word, &value, value + 1, false,
                                        __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return true;
    return false;
}
inline bool try_write(uint32_t* word, uint32_t owner = kWriter) {
    uint32_t expected = 0;
    return __atomic_compare_exchange_n(word, &expected, owner, false,
                                       __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
}
struct RingRead {
    uint32_t* word = nullptr;
    explicit RingRead(uint32_t* p) : word(try_read(p) ? p : nullptr) {}
    ~RingRead() { if (word) __atomic_sub_fetch(word, 1, __ATOMIC_RELEASE); }
    explicit operator bool() const { return word != nullptr; }
    RingRead(const RingRead&) = delete;
    RingRead& operator=(const RingRead&) = delete;
};
struct RingWrite {
    uint32_t* word = nullptr;
    explicit RingWrite(uint32_t* p, uint32_t owner = kWriter) : word(try_write(p, owner) ? p : nullptr) {}
    ~RingWrite() { if (word) __atomic_store_n(word, 0u, __ATOMIC_RELEASE); }
    explicit operator bool() const { return word != nullptr; }
    RingWrite(const RingWrite&) = delete;
    RingWrite& operator=(const RingWrite&) = delete;
};

// Requires exclusive flock ownership of the ring fd. Camera reader counts and
// camera-owned metadata locks must survive recovery; only the dead producer's
// tokens may be cleared. Incomplete slots lose their sequence before unlocking.
inline void recover_dead_producer(ShmHeader& header) {
    for (auto& lane : header.lane) {
        for (uint32_t slot = 0; slot < recam::kLaneBufs; ++slot) {
            if (__atomic_load_n(&lane.slots[slot], __ATOMIC_ACQUIRE) == recam::kWriter) {
                lane.slotSeq[slot] = 0;
                __atomic_store_n(&lane.slots[slot], 0u, __ATOMIC_RELEASE);
            }
        }
        if (__atomic_load_n(&lane.access, __ATOMIC_ACQUIRE) == recam::kWriter + 1) {
            lane.published = 0;
            __atomic_store_n(&lane.width, 0u, __ATOMIC_RELEASE);
            lane.height = 0; lane.bufBytes = 0;
            if (!lane.reqHeight) __atomic_store_n(&lane.reqWidth, 0u, __ATOMIC_RELEASE);
            __atomic_store_n(&lane.access, 0u, __ATOMIC_RELEASE);
        }
    }

}

// Holding lane shared access also prevents geometry/offset changes. A delayed
// reader can keep its old publication pinned while newer ones pass through the
// other slots. Sequence gaps encode skipped slots and are not lost input frames.
struct RingFrame {
    ShmLane& lane;
    uint64_t seq = 0;
    uint32_t slot = 0;
    explicit RingFrame(ShmLane& l) : lane(l) {
        for (unsigned i = 0; i < 8; ++i) {
            const uint64_t candidate = __atomic_load_n(&l.published, __ATOMIC_ACQUIRE);
            if (!candidate) break;
            const uint32_t index = (candidate - 1) % kLaneBufs;
            if (!try_read(&l.slots[index])) continue;
            if (l.slotSeq[index] == candidate) { seq = candidate; slot = index; break; }
            __atomic_sub_fetch(&l.slots[index], 1, __ATOMIC_RELEASE);
        }
    }
    ~RingFrame() { if (seq) __atomic_sub_fetch(&lane.slots[slot], 1, __ATOMIC_RELEASE); }
    RingFrame(const RingFrame&) = delete;
    RingFrame& operator=(const RingFrame&) = delete;
};

struct RingPublication {
    ShmLane& lane;
    uint64_t seq = 0;
    uint32_t slot = 0;
    explicit RingPublication(ShmLane& l) : lane(l) {
        const uint64_t previous = __atomic_load_n(&l.published, __ATOMIC_ACQUIRE);
        for (uint32_t i = 1; i <= kLaneBufs; ++i) {
            const uint64_t next = previous + i;
            const uint32_t index = (next - 1) % kLaneBufs;
            if (previous && index == (previous - 1) % kLaneBufs) continue;
            if (try_write(&l.slots[index])) {
                // An aborted scale or dead producer must not leave an old ID
                // attached to partially overwritten pixels.
                l.slotSeq[index] = 0;
                seq = next; slot = index; break;
            }
        }
    }
    void publish(uint64_t now) {
        lane.slotSeq[slot] = seq;
        lane.slotNs[slot] = now;
        __atomic_store_n(&lane.publishedNs, now, __ATOMIC_RELEASE);
        // Release the pixels before advertising them: the current slot must
        // always be readable. The single producer cannot reacquire it here.
        __atomic_store_n(&lane.slots[slot], 0u, __ATOMIC_RELEASE);
        __atomic_store_n(&lane.published, seq, __ATOMIC_RELEASE);
        committed = true;
    }
    ~RingPublication() {
        if (seq && !committed) __atomic_store_n(&lane.slots[slot], 0u, __ATOMIC_RELEASE);
    }
    RingPublication(const RingPublication&) = delete;
    RingPublication& operator=(const RingPublication&) = delete;
private:
    bool committed = false;
};
}  // namespace recam
