// recam_status - machine-readable state, for a UI to poll.

#include <fcntl.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <cstdio>

#include "target.hpp"
#include "../common/control.hpp"
#include "../common/shmframe.hpp"

namespace t = recam::target;

// Read-only and ptrace-free, so polling this can never disturb the camera.
static bool read_at(pid_t pid, uintptr_t addr, void* out, size_t len)
{
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/mem", pid);
    const int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;

    const ssize_t n = pread(fd, out, len, static_cast<off_t>(addr));
    close(fd);
    return n == static_cast<ssize_t>(len);
}

static bool read_ring(pid_t pid, uint32_t shmFd, recam::ShmHeader* out)
{
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/fd/%u", pid, shmFd);
    const int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;

    const ssize_t n = pread(fd, out, sizeof(*out), 0);
    close(fd);
    return n == static_cast<ssize_t>(sizeof(*out)) && out->magic == recam::kShmMagic;
}

static uint64_t now_ns()
{
    struct timespec ts {};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull + static_cast<uint64_t>(ts.tv_nsec);
}

static const char* yn(bool b) { return b ? "true" : "false"; }

int main()
{
    const uint64_t now = now_ns();
    const pid_t    pid = t::find(t::kProcName);

    printf("{\n");
    printf("  \"now_ns\": %llu,\n", static_cast<unsigned long long>(now));
    // Without root, an uninjected process looks the same as one we cannot see into.
    printf("  \"root\": %s,\n", yn(geteuid() == 0));
    printf("  \"cameraserver\": { \"running\": %s, \"pid\": %d },\n",
           yn(pid > 0), pid > 0 ? pid : 0);

    const pid_t feeder = t::find("recam_feed");
    printf("  \"feeder\": { \"running\": %s, \"pid\": %d },\n",
           yn(feeder > 0), feeder > 0 ? feeder : 0);

    recam::ControlBlock cb{};
    bool haveCb = false;
    uintptr_t ctrlAddr = 0;

    if (pid > 0) {
        ctrlAddr = t::find_control_block(pid);
        if (ctrlAddr && read_at(pid, ctrlAddr, &cb, sizeof(cb)))
            haveCb = (cb.magic == recam::kControlMagic);
    }

    printf("  \"payload\": {\n");
    printf("    \"injected\": %s,\n", yn(haveCb));
    if (haveCb) {
        printf("    \"control_version\": %u,\n", cb.version);
        printf("    \"speaks_version\": %u,\n", recam::kControlVersion);
        printf("    \"active\": %s,\n", yn(cb.active != 0));
        printf("    \"generation\": %u,\n", cb.generation);
        printf("    \"hooks_installed\": %u,\n", cb.hooks_installed);
        printf("    \"owner_pid\": %u,\n", cb.owner_pid);
        printf("    \"owner_base\": \"0x%llx\",\n",
               static_cast<unsigned long long>(cb.owner_base));
        printf("    \"control_addr\": \"0x%llx\"\n",
               static_cast<unsigned long long>(ctrlAddr));
    } else {
        printf("    \"active\": false\n");
    }
    printf("  },\n");

    recam::ShmHeader h{};
    const bool haveRing = haveCb && cb.shm_fd && read_ring(pid, cb.shm_fd, &h);

    // Re-sample: a live feeder can stamp a lane after the clock read at the top.
    const uint64_t nowAge = now_ns();

    printf("  \"ring\": {\n");
    printf("    \"present\": %s", yn(haveRing));
    if (haveRing) {
        printf(",\n    \"version\": %u,\n", h.version);
        printf("    \"speaks_version\": %u,\n", recam::kShmVersion);
        printf("    \"bytes\": %llu,\n", static_cast<unsigned long long>(cb.shm_size));
        // Counters are cumulative; poll twice and difference them for a rate.
        printf("    \"published\": %llu,\n", static_cast<unsigned long long>(h.published));
        printf("    \"consumed\": %llu,\n", static_cast<unsigned long long>(h.consumed));
        printf("    \"dropped\": %llu,\n", static_cast<unsigned long long>(h.dropped));
        printf("    \"lanes\": [");

        bool first = true;
        for (uint32_t i = 0; i < recam::kShmLanes; i++) {
            const recam::ShmLane& l = h.lane[i];
            if (!l.reqWidth && !l.width) continue;

            long long ageMs = -1;
            if (l.publishedNs)
                ageMs = (l.publishedNs >= nowAge)
                    ? 0 : static_cast<long long>((nowAge - l.publishedNs) / 1000000ull);

            printf("%s\n      { \"index\": %u, \"req_width\": %u, \"req_height\": %u, "
                   "\"width\": %u, \"height\": %u, \"served\": %s, "
                   "\"published\": %llu, \"age_ms\": %lld }",
                   first ? "" : ",", i, l.reqWidth, l.reqHeight, l.width, l.height,
                   yn(l.width == l.reqWidth && l.width != 0),
                   static_cast<unsigned long long>(l.published), ageMs);
            first = false;
        }
        printf("%s]\n", first ? "" : "\n    ");
    } else {
        printf("\n");
    }
    printf("  }\n");
    printf("}\n");
    return 0;
}
