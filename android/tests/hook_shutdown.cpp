// Deterministically interleave shutdown with a callback's FrameSource::fill().
#include "../src/payload/framehook.cpp"
#include <cassert>
#include <cstdio>
#include <atomic>
#include <thread>
#include <vector>

namespace recam {
static uint8_t pixels[6];
static unsigned destroyed = 0;
static bool remove_ok = true;
void shmsource_destroy() { ++destroyed; }
bool bufferlock_have_ycbcr() { return true; }
bool bufferlock_init() { return true; }
bool bufferlock_acquire(void*, int32_t, LockedBuffer* out) {
    out->plane[0] = {pixels, 2, 1};
    out->plane[1] = {pixels + 4, 1, 1};
    out->plane[2] = {pixels + 5, 1, 1};
    return true;
}
bool bufferlock_acquire_handle(const void*, int32_t, int32_t, int32_t, LockedBuffer*) { return false; }
void bufferlock_release(LockedBuffer*) {}
FrameSource* shmsource_get() { return nullptr; }
FrameSource* colorbars_source() { return nullptr; }
HookResult hook_install(uintptr_t, void*, InlineHook* h) { h->installed = true; return HookResult::Ok; }
HookResult hook_remove(InlineHook* hook) { if (!remove_ok) return HookResult::TextWriteFailed; hook->installed = false; return HookResult::Ok; }
const char* hook_result_str(HookResult) { return "test"; }
}

class ShutdownSource : public recam::FrameSource {
public:
    const char* name() const override { return "shutdown test"; }
    bool fill(const recam::FrameTarget&) override {
        assert(recam::framehook_remove());
        assert(recam::destroyed == 0); // fill still owns the mapping
        return true;
    }
};
static int64_t original(void*, const void*, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t) { return 42; }

int main() {
    static ShutdownSource source;
    recam::g_shm = &source;
    recam::g_enabled = 1;
    recam::g_orig = original;
    recam::g_hook.installed = true;
    recam::remove_ok = false;
    assert(!recam::framehook_remove() && recam::g_enabled && !recam::destroyed);
    recam::remove_ok = true;
    alignas(8) uint8_t anw[112]{};
    int32_t dimension = 2, format = 0x23;
    memcpy(anw + recam::kAnwWidthOff, &dimension, sizeof(dimension));
    memcpy(anw + recam::kAnwHeightOff, &dimension, sizeof(dimension));
    memcpy(anw + recam::kAnwFormatOff, &format, sizeof(format));
    recam::StreamBufferView buffer{};
    buffer.buffer = reinterpret_cast<void**>(anw + recam::kAnwHandleOff);
    buffer.releaseFence = -1;
    assert(recam::recam_on_return_buffer(nullptr, &buffer, 0, 0, 0, 0, 0, 0) == 42);
    assert(!recam::g_hook.installed && recam::g_shm == &source);
    assert(recam::destroyed == 1);
    // A callback that had branched into the old image but entered late forwards
    // to the retained trampoline without touching the released frame source.
    assert(recam::recam_on_return_buffer(nullptr, &buffer, 0, 0, 0, 0, 0, 0) == 42);
    assert(recam::framehook_remove() && recam::destroyed == 1);
    // Exercise the last-callback election with multiple simultaneously held
    // references. Only the last departure may reclaim, exactly once.
    recam::g_enabled=1;recam::g_lifecycle=recam::Running;recam::destroyed=0;
    recam::g_hook.installed=true;
    std::atomic<unsigned> arrived{0};std::atomic<bool> leave{false};
    std::vector<std::thread> threads;
    for(unsigned i=0;i<16;++i) threads.emplace_back([&] {
        recam::CallbackScope scope;assert(scope.substitute);
        ++arrived;while(!leave.load()) std::this_thread::yield();
    });
    while(arrived.load()!=16)std::this_thread::yield();
    assert(recam::framehook_remove() && recam::destroyed==0);
    leave=true;for(auto& thread:threads)thread.join();
    assert(recam::destroyed==1 && recam::g_callbacks==0);
    // Reuse the immutable original after retirement. Old, delayed callbacks can
    // enter during prepare but must neither substitute nor destroy a new ring.
    recam::g_hook.target = 0x1000;
    assert(!recam::framehook_prepare(0x2000));
    assert(recam::framehook_prepare(0x1000));
    assert(recam::recam_on_return_buffer(nullptr, &buffer, 0, 0, 0, 0, 0, 0) == 42);
    assert(recam::destroyed == 1 && recam::g_lifecycle == recam::Preparing);
    for (unsigned i = 0; i < 100; ++i) {
        assert(recam::framehook_install(0x1000));
        assert(recam::g_orig == original);
        {
            recam::CallbackScope scope;
            assert(recam::framehook_remove());
            assert(!recam::framehook_prepare(0x1000));
        }
        assert(recam::g_lifecycle == recam::Dormant);
        assert(recam::destroyed == i + 2);
        assert(recam::framehook_prepare(0x1000));
    }
    // A never-installed image can release a failed-init ring without retirement.
    recam::g_orig=nullptr;recam::g_lifecycle=recam::Running;
    assert(recam::framehook_remove() && recam::g_lifecycle==recam::Dormant);
    puts("in-flight shutdown, failure preservation, and concurrent reclamation checks passed");
}
