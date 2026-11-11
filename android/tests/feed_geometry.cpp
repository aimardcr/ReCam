// Run as root with the active ring's /proc/PID/fd/FD path, no camera clients.
// Uses the actual feeder and production consumer; it does not change camera state.
#include "../src/payload/shmsource.cpp"
#include <cassert>
#include <cstdio>
#include <fcntl.h>
#include <vector>

int main(int argc, char** argv) {
    using namespace recam;
    assert(argc == 2);
    int fd = open(argv[1], O_RDWR | O_CLOEXEC);
    assert(fd >= 0);
    g_size = shm_total_bytes();
    g_base = static_cast<uint8_t*>(mmap(nullptr, g_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));
    assert(g_base != MAP_FAILED);
    assert(header()->magic == kShmMagic && header()->version == kShmVersion);
    for (auto dims : {std::pair<int,int>{1960,882}, {4608,3456}, {4000,3000}, {1080,1920}}) {
        const int w = dims.first, h = dims.second;
        const size_t bytes = static_cast<size_t>(w) * h * 3 / 2;
        std::vector<uint8_t> pixels(bytes + 64, 0xA5);
        FrameTarget t;
        t.width = w; t.height = h; t.format = 0x23;
        t.y = {pixels.data() + 32, w, 1};
        t.cb = {t.y.data + w * h, w / 2, 1};
        t.cr = {t.cb.data + w * h / 4, w / 2, 1};
        const uint64_t deadline = now_ns() + 5000000000ull;
        bool filled = false;
        while (!(filled = g_source.fill(t)) && now_ns() < deadline) usleep(10000);
        assert(filled);
        const uint64_t consumed = header()->consumed;
        uint64_t first = 0;
        for (size_t i = 32; i < bytes + 32; ++i) first = first * 31 + pixels[i];
        usleep(80000);
        const uint64_t start = now_ns();
        assert(g_source.fill(t));
        const uint64_t copyNs = now_ns() - start;
        uint64_t second = 0;
        for (size_t i = 32; i < bytes + 32; ++i) second = second * 31 + pixels[i];
        assert(first != second && header()->consumed > consumed);
        for (size_t i = 0; i < 32; ++i) assert(pixels[i] == 0xA5 && pixels[bytes + 32 + i] == 0xA5);
        uint32_t sw, sh; assert(lane_geometry(w, h, sw, sh));
        printf("%dx%d from %ux%u: moving pattern consumed; one copy %.3f ms\n", w, h, sw, sh, copyNs / 1e6);
    }
    munmap(g_base, g_size); g_base = nullptr; g_size = 0;
    close(fd);
}
