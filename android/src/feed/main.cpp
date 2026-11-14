#include "../cli/commands.hpp"

// recam feed - fills the payload's shared frame ring.

#include <fcntl.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <time.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include <KittyMemoryMgr.hpp>
#include "target.hpp"
#include "decoder.hpp"
#include "scale.hpp"
#include "../common/control.hpp"
#include "../common/shmframe.hpp"
#include "../common/ring_access.hpp"

namespace t = recam::target;
using recam::ShmHeader;
using recam::ShmLane;

static volatile sig_atomic_t g_stop = 0;
static void on_signal(int) { g_stop = 1; }

static uint64_t now_ns()
{
    struct timespec ts {};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull + static_cast<uint64_t>(ts.tv_nsec);
}

// ---------------------------------------------------------------- control block

// No ptrace: the payload publishes here so a feeder can attach to a live process.
static bool read_control(pid_t pid, uintptr_t addr, recam::ControlBlock* out)
{
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/mem", pid);
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) { KITTY_LOGE("feed: open %s: %s", path, strerror(errno)); return false; }

    ssize_t n = pread(fd, out, sizeof(*out), static_cast<off_t>(addr));
    close(fd);

    if (n != static_cast<ssize_t>(sizeof(*out))) {
        KITTY_LOGE("feed: short read of the control block");
        return false;
    }
    if (out->magic != recam::kControlMagic) {
        KITTY_LOGE("feed: bad control magic 0x%llx",
                   static_cast<unsigned long long>(out->magic));
        return false;
    }
    if (out->version != recam::kControlVersion) {
        KITTY_LOGE("feed: control block is v%u, this tool speaks v%u - rebuild and "
                   "re-inject the payload.", out->version, recam::kControlVersion);
        return false;
    }
    return true;
}

// ---------------------------------------------------------------- frame source

// Where the UI stages the clip to play; absent means the built-in test pattern.
static const char* const kDefaultSource = "/data/local/tmp/recam_source.mp4";

static recam::VideoDecoder g_decoder;
static bool                g_haveVideo = false;

// Localhost only: reached from a desktop through `adb forward`, never off-device.
static const uint16_t kListenPort = 27183;

static recam::VideoDecoder g_stream;
static bool                g_streamLive = false;
static int                 g_listenFd   = -1;

static void start_listener()
{
    g_listenFd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (g_listenFd < 0) { KITTY_LOGE("feed: socket: %s", strerror(errno)); return; }

    int on = 1;
    setsockopt(g_listenFd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

    sockaddr_in a{};
    a.sin_family      = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port        = htons(kListenPort);

    if (bind(g_listenFd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0 ||
        listen(g_listenFd, 1) != 0) {
        KITTY_LOGE("feed: cannot listen on 127.0.0.1:%u: %s", kListenPort, strerror(errno));
        close(g_listenFd);
        g_listenFd = -1;
        return;
    }
    KITTY_LOGI("feed: listening on 127.0.0.1:%u - adb forward tcp:%u tcp:%u, then send "
               "H.264 in FLV or Annex-B", kListenPort, kListenPort, kListenPort);
}

// A live stream outranks the file; hanging up falls straight back to it.
static void poll_listener()
{
    if (g_listenFd < 0 || g_streamLive) return;

    const int fd = accept4(g_listenFd, nullptr, nullptr, SOCK_CLOEXEC);
    if (fd < 0) {
        static int warned = 0;
        if (errno != EAGAIN && errno != EWOULDBLOCK && warned++ < 3)
            KITTY_LOGE("feed: accept4: %s", strerror(errno));
        return;
    }

    KITTY_LOGI("feed: a sender connected; the stream now outranks the file");
    g_streamLive = g_stream.open_stream(fd);
    if (!g_streamLive) close(fd);
}

static void produce_pattern(uint8_t* dst, uint32_t w, uint32_t h, uint64_t frameNo)
{
    struct Bar { uint8_t y, u, v; };
    static const Bar kBars[8] = {
        {255,128,128},{226,  0,149},{179,171,  0},{150, 44, 21},
        {105,212,235},{ 76, 85,255},{ 29,255,107},{  0,128,128},
    };

    const uint32_t shift = static_cast<uint32_t>((frameNo * 8) % w);
    uint8_t* y = dst;
    uint8_t* u = y + static_cast<size_t>(w) * h;
    uint8_t* v = u + static_cast<size_t>(w / 2) * (h / 2);

    // Luma: one row is enough, then replicate.
    std::vector<uint8_t> rowY(w);
    for (uint32_t x = 0; x < w; x++)
        rowY[x] = kBars[(((x + shift) % w) * 8) / w].y;
    for (uint32_t r = 0; r < h; r++)
        memcpy(y + static_cast<size_t>(r) * w, rowY.data(), w);

    // A moving marker makes a stalled feed obvious at a glance.
    const uint32_t bs = h / 10;
    const uint32_t bx = static_cast<uint32_t>((frameNo * 7) % (w - bs));
    const uint32_t by = static_cast<uint32_t>((frameNo * 3) % (h - bs));
    for (uint32_t r = by; r < by + bs; r++)
        memset(y + static_cast<size_t>(r) * w + bx, 255, bs);

    // Chroma at half resolution.
    const uint32_t cw = w / 2, ch = h / 2;
    std::vector<uint8_t> rowU(cw), rowV(cw);
    for (uint32_t x = 0; x < cw; x++) {
        const Bar& b = kBars[((((x * 2) + shift) % w) * 8) / w];
        rowU[x] = b.u;
        rowV[x] = b.v;
    }
    for (uint32_t r = 0; r < ch; r++) {
        memcpy(u + static_cast<size_t>(r) * cw, rowU.data(), cw);
        memcpy(v + static_cast<size_t>(r) * cw, rowV.data(), cw);
    }
}

// A lane nobody has asked about for this long is handed back to the free pool.
static const uint64_t kLaneIdleNs = 2ull * 1000 * 1000 * 1000;

// Answer new size requests and retire abandoned lanes.
static void service_lanes(ShmHeader* h, uint64_t nowN)
{
    for (uint32_t i = 0; i < recam::kShmLanes; i++) {
        recam::ShmLane& l = h->lane[i];
        // Fast path only reads atomic metadata. Exclusive access is needed for
        // reconfiguration, never for normal publications or camera copies.
        const uint32_t requested = __atomic_load_n(&l.reqWidth, __ATOMIC_ACQUIRE);
        if (!requested) continue;
        const uint64_t stamp = __atomic_load_n(&l.reqNs, __ATOMIC_ACQUIRE);
        const bool idle = nowN > stamp && nowN - stamp > kLaneIdleNs;
        if (!idle && __atomic_load_n(&l.width, __ATOMIC_ACQUIRE) != 0) continue;
        recam::RingWrite access(&l.access, recam::kWriter + 1);
        if (!access) continue;
        const uint32_t rw = l.reqWidth, rh = l.reqHeight;
        if (!rw || !rh) continue;
        const uint64_t current = __atomic_load_n(&l.reqNs, __ATOMIC_ACQUIRE);
        if (nowN > current && nowN - current > kLaneIdleNs) {
            KITTY_LOGI("feed: retiring lane %u (%ux%u), nobody is asking", i, rw, rh);
            l.published = 0;
            __atomic_store_n(&l.width, 0u, __ATOMIC_RELEASE); l.height = 0; l.bufBytes = 0;
            l.reqHeight = 0;
            __atomic_store_n(&l.reqWidth, 0u, __ATOMIC_RELEASE);
            continue;
        }

        if (recam::lane_configured(l)) continue;

        uint32_t storedW, storedH;
        if (!recam::lane_geometry(rw, rh, storedW, storedH)) {
            // Invalid geometry is rejected before lane claims by the payload.
            // Avoid per-frame warnings if a malformed request nevertheless arrives.
            continue;
        }

        l.published = 0;
        l.publishedNs = 0;
        l.bufBytes  = recam::lane_frame_bytes(storedW, storedH);
        l.height    = storedH;
        __atomic_store_n(&l.width, storedW, __ATOMIC_RELEASE);
        KITTY_LOGI("feed: lane %u now serving %ux%u from %ux%u stored pixels",
                   i, rw, rh, storedW, storedH);
    }
}

// ---------------------------------------------------------------- main

enum class Outcome { Stopped, Reattach, Error };

static Outcome attach_and_feed()
{
    const pid_t pid = t::find(t::kProcName);
    if (pid <= 0) { KITTY_LOGE("feed: %s is not running.", t::kProcName); return Outcome::Error; }

    const uintptr_t ctrlAddr = t::find_control_block(pid);
    if (!ctrlAddr) {
        KITTY_LOGE("feed: no payload loaded in %s. Run recam inject first.", t::kProcName);
        return Outcome::Error;
    }

    recam::ControlBlock cb{};
    if (!read_control(pid, ctrlAddr, &cb)) return Outcome::Error;
    if (!cb.shm_fd || !cb.shm_size) {
        KITTY_LOGE("feed: the payload published no frame ring.");
        return Outcome::Error;
    }

    // Opening this path yields a new fd to the same object; no fd passing needed.
    char shmPath[64];
    snprintf(shmPath, sizeof(shmPath), "/proc/%d/fd/%u", pid, cb.shm_fd);
    int fd = open(shmPath, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        KITTY_LOGE("feed: open %s: %s", shmPath, strerror(errno));
        return Outcome::Error;
    }

    // Independent opens of the same ring must not publish concurrently. Keep
    // the lock for this attachment; the kernel releases it on SIGKILL.
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        KITTY_LOGE("feed: another producer owns this frame ring");
        close(fd);
        return Outcome::Error;
    }
    struct RingFd { int fd; ~RingFd() { close(fd); } } ringFd{fd};
    void* base = mmap(nullptr, cb.shm_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (base == MAP_FAILED) { KITTY_LOGE("feed: mmap: %s", strerror(errno)); return Outcome::Error; }

    ShmHeader* h = static_cast<ShmHeader*>(base);
    if (h->magic != recam::kShmMagic || h->version != recam::kShmVersion ||
        h->laneCount != recam::kShmLanes || cb.shm_size != recam::shm_total_bytes()) {
        KITTY_LOGE("feed: bad ring magic/version");
        munmap(base, cb.shm_size);
        return Outcome::Error;
    }

    recam::recover_dead_producer(*h);

    // Desktop verifies this acknowledgement against recam status before reporting
    // success. Keep stdout available even when platform logcat omits an error.
    printf("RECAM_FEED_READY pid=%d generation=%u\n", getpid(), cb.generation);
    fflush(stdout);

    KITTY_LOGI("feed: attached to %s (pid %d) ring, %llu bytes, %u lanes",
               t::kProcName, pid, static_cast<unsigned long long>(cb.shm_size),
               h->laneCount);
    KITTY_LOGI("feed: source = %s, plus anything that connects. Ctrl-C to stop.",
               g_haveVideo ? "decoded video" : "built-in test pattern");

    uint64_t frameNo = 0;
    uint64_t lastLog = 0;
    uint64_t lastCheck = now_ns();
    uint64_t deadline = now_ns();
    uint64_t lastLiveFrameNs = deadline;
    Outcome out = Outcome::Stopped;

    const uint64_t intervalNs = g_haveVideo
        ? static_cast<uint64_t>(g_decoder.frame_interval_us()) * 1000ull
        : 33333000ull;

    while (!g_stop) {
        service_lanes(h, now_ns());

        const bool wasLive = g_streamLive;
        poll_listener();
        if (!wasLive && g_streamLive) lastLiveFrameNs = now_ns();

        // One decode feeds every lane; only the scale is paid per size.
        recam::SrcFrame src;
        bool haveSrc = false;
        recam::VideoDecoder* used = nullptr;

        if (g_streamLive) {
            haveSrc = g_stream.next(&src);
            if (haveSrc) {
                used = &g_stream;
                lastLiveFrameNs = now_ns();
                deadline = lastLiveFrameNs;
            }
            // A returned frame owns pointers into the decoder: close only after
            // scaling it, even when EOF arrived in the same pump.
            if (g_stream.stream_ended() && !haveSrc) {
                KITTY_LOGI("feed: the sender hung up; falling back");
                g_stream.close();
                g_streamLive = false;
                deadline = now_ns();
            }
        }
        const uint64_t now = now_ns();
        const bool fallbackDue = !haveSrc && (!g_streamLive ||
            (now - lastLiveFrameNs >= 2000000000ull && now >= deadline));
        if (fallbackDue && g_streamLive && now > deadline + intervalNs)
            deadline = now; // restart the fallback clock after a live gap
        if (fallbackDue && g_haveVideo) {
            haveSrc = g_decoder.next(&src);
            if (haveSrc) used = &g_decoder;
        }

        // A live stream with no new output leaves the last publication intact.
        // Never replace it with a pattern merely because decoding is asynchronous.
        const bool publishFrame = haveSrc || fallbackDue;
        uint32_t served = 0;
        for (uint32_t i = 0; publishFrame && i < recam::kShmLanes; i++) {
            recam::ShmLane& l = h->lane[i];
            // Only this feeder configures an occupied lane; the payload claims
            // free lanes but never edits a served lane's geometry.
            const uint32_t lw = l.width, lh = l.height;
            if (!recam::lane_configured(l)) continue;

            recam::RingPublication publication(l);
            if (!publication.seq) {
                __atomic_add_fetch(&h->producerSkipped, 1, __ATOMIC_RELAXED);
                continue; // preserve the last complete frame; never wait on a reader
            }
            uint8_t* buf = static_cast<uint8_t*>(base) + l.dataOffset +
                           publication.slot * l.bufBytes;

            if (haveSrc) {
                if (!recam::scale_to_i420(src, buf, lw, lh)) continue;
            } else {
                produce_pattern(buf, lw, lh, frameNo);
            }

            // Publish only after the pixels are in place.
            const uint64_t publishedNs = now_ns();
            publication.publish(publishedNs);
            served++;
        }
        if (used) used->release();

        if (publishFrame) frameNo++;
        if (g_streamLive && g_stream.stream_ended()) {
            g_stream.close();
            g_streamLive = false;
            deadline = now_ns();
        }
        if (g_streamLive && fallbackDue) {
            deadline += intervalNs;
            const uint64_t after = now_ns();
            if (deadline < after) deadline = after;
        }
        __atomic_add_fetch(&h->published, served, __ATOMIC_RELAXED);

        // A re-injected payload creates a new memfd; this mapping goes stale.
        if (now_ns() - lastCheck > 1000000000ull) {
            lastCheck = now_ns();
            recam::ControlBlock cur{};
            if (!read_control(pid, ctrlAddr, &cur) || cur.shm_fd != cb.shm_fd || cur.generation != cb.generation) {
                KITTY_LOGW("feed: the payload republished its ring - re-attaching.");
                out = Outcome::Reattach;
                break;
            }
        }

        if (now_ns() - lastLog > 5ull * 1000000000ull) {
            lastLog = now_ns();
            KITTY_LOGI("feed: %llu published, %llu consumed, %llu dropped, %u lane(s) live",
                       static_cast<unsigned long long>(h->published),
                       static_cast<unsigned long long>(h->consumed),
                       static_cast<unsigned long long>(h->dropped), served);
        }

        if (g_streamLive) {
            // Socket readiness wakes us immediately; codec output is checked with
            // a 2 ms poll timeout. Live publication is not tied to the file tick.
            if (!haveSrc) g_stream.wait_stream(2);
            continue;
        }

        // File/pattern playback follows the source frame interval.
        deadline += intervalNs;
        const uint64_t after = now_ns();
        if (deadline > after) usleep(static_cast<useconds_t>((deadline - after) / 1000));
        else                  deadline = after;   // behind: drop the debt, do not spiral
    }

    munmap(base, cb.shm_size);
    if (out == Outcome::Stopped)
        KITTY_LOGI("feed: stopping after %llu frames. The payload falls back to colour "
                   "bars about 2s from now.", static_cast<unsigned long long>(frameNo));
    return out;
}

int recam::cli::feed(int argc, char** argv)
{
    if (getuid() != 0) { KITTY_LOGE("Must run as root."); return 1; }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGHUP, SIG_IGN);

    const char* src = (argc > 1) ? argv[1] : kDefaultSource;
    if (access(src, R_OK) == 0) {
        g_haveVideo = g_decoder.open(src);
        if (!g_haveVideo)
            KITTY_LOGW("feed: %s could not be decoded; using the test pattern.", src);
    } else {
        KITTY_LOGI("feed: no %s; using the test pattern.", src);
    }
    start_listener();
    if (g_listenFd < 0) {
        fprintf(stderr, "feed: could not listen on port %u; see device log\n", kListenPort);
        g_decoder.close();
        return 1;
    }

    int rc = 0;
    while (!g_stop) {
        const Outcome o = attach_and_feed();
        if (o == Outcome::Stopped) break;
        if (o == Outcome::Error)   { rc = 1; break; }
        usleep(200000);   // the payload was just re-injected; let it settle
    }

    g_stream.close();
    g_decoder.close();
    if (g_listenFd >= 0) close(g_listenFd);
    return rc;
}
