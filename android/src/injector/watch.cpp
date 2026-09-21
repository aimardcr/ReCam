// recam_watch - keep the payload loaded across cameraserver restarts.

#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <poll.h>
#include <signal.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <limits.h>
#include <time.h>

#include <cstdio>
#include <cstdlib>
#include <string>

#include <KittyMemoryMgr.hpp>
#include "target.hpp"

namespace t = recam::target;

static const char* kInjectName = "recam_inject";

// Do not re-inject as fast as a crash loop restarts.
static const int kMaxConsecutiveFailures = 5;
static const int kBackoffStartMs         = 1000;
static const int kBackoffMaxMs           = 30000;

// How long to wait for init to bring cameraserver back before giving up on a cycle.
static const int kRespawnTimeoutMs = 60000;
static const int kRespawnPollMs    = 200;

static volatile sig_atomic_t g_stop = 0;

static void on_signal(int) { g_stop = 1; }

static void sleep_ms(int ms)
{
    struct timespec ts { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&ts, nullptr);
}

// ------------------------------------------------------------------ run the injector

// Path to recam_inject, which lives next to this binary.
static std::string injector_path()
{
    char exe[PATH_MAX] = {0};
    ssize_t n = ::readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n <= 0) return kInjectName;

    std::string p(exe, static_cast<size_t>(n));
    size_t slash = p.rfind('/');
    return (slash == std::string::npos) ? std::string(kInjectName)
                                        : p.substr(0, slash + 1) + kInjectName;
}

// fork+exec, not system(): no shell, and a clean exit status.
static int run_injector(const std::string& path)
{
    pid_t child = fork();
    if (child < 0) {
        KITTY_LOGE("watch: fork failed: %s", strerror(errno));
        return -1;
    }
    if (child == 0) {
        execl(path.c_str(), path.c_str(), static_cast<char*>(nullptr));
        _exit(127);   // exec failed
    }

    int status = 0;
    while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) {
        KITTY_LOGE("watch: injector killed by signal %d", WTERMSIG(status));
        return -1;
    }
    return -1;
}

// ------------------------------------------------------------------ waiting

static int pidfd_open_compat(pid_t pid)
{
#ifdef SYS_pidfd_open
    return static_cast<int>(syscall(SYS_pidfd_open, pid, 0u));
#else
    (void)pid;
    errno = ENOSYS;
    return -1;
#endif
}

// Block until `pid` exits. Returns false only if we were asked to stop.
static bool wait_for_exit(pid_t pid)
{
    int pfd = pidfd_open_compat(pid);

    if (pfd >= 0) {
        // Event-driven: poll() returns as soon as the process is reaped.
        for (;;) {
            struct pollfd p { pfd, POLLIN, 0 };
            int rc = poll(&p, 1, 1000);          // 1s tick so ^C is responsive
            if (g_stop) { close(pfd); return false; }
            if (rc < 0 && errno == EINTR) continue;
            if (rc > 0) { close(pfd); return true; }
        }
    }

    // No pidfd (pre-5.3 kernel): fall back to checking /proc.
    KITTY_LOGW("watch: pidfd_open unavailable (%s); falling back to polling",
               strerror(errno));
    while (!g_stop) {
        if (t::proc_name(pid) != t::kProcName) return true;   // gone or recycled
        sleep_ms(500);
    }
    return false;
}

// Wait for init to bring cameraserver back. Returns its new pid, or -1.
static pid_t wait_for_respawn(pid_t oldPid)
{
    for (int waited = 0; waited < kRespawnTimeoutMs && !g_stop; waited += kRespawnPollMs) {
        pid_t p = t::find(t::kProcName);
        if (p > 0 && p != oldPid) return p;
        sleep_ms(kRespawnPollMs);
    }
    return -1;
}

// ------------------------------------------------------------------ main

int main()
{
    if (getuid() != 0) {
        KITTY_LOGE("Must run as root.");
        return 1;
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    // A dead adb shell must not take the watcher with it when it is backgrounded.
    signal(SIGHUP, SIG_IGN);

    const std::string inject = injector_path();
    KITTY_LOGI("watch: supervising %s using %s", t::kProcName, inject.c_str());

    int failures = 0;
    int backoff  = kBackoffStartMs;

    while (!g_stop) {
        pid_t pid = t::find(t::kProcName);
        if (pid <= 0) {
            KITTY_LOGW("watch: %s is not running; waiting for it.", t::kProcName);
            pid = wait_for_respawn(-1);
            if (pid <= 0) {
                if (g_stop) break;
                KITTY_LOGE("watch: %s did not appear within %ds.",
                           t::kProcName, kRespawnTimeoutMs / 1000);
                return 1;
            }
        }

        KITTY_LOGI("watch: injecting into %s (pid %d) ...", t::kProcName, pid);
        const int rc = run_injector(inject);

        if (rc == 0) {
            failures = 0;
            backoff  = kBackoffStartMs;
            KITTY_LOGI("watch: payload live in pid %d; waiting for it to exit.", pid);
        } else {
            if (++failures >= kMaxConsecutiveFailures) {
                KITTY_LOGE("watch: injector failed %d times in a row (last rc=%d). "
                           "Giving up rather than fighting a crash loop - fix the "
                           "cause and start the watcher again.", failures, rc);
                return 1;
            }
            KITTY_LOGW("watch: injector returned %d (failure %d/%d); retrying in %dms.",
                       rc, failures, kMaxConsecutiveFailures, backoff);
            sleep_ms(backoff);
            backoff = (backoff * 2 > kBackoffMaxMs) ? kBackoffMaxMs : backoff * 2;
            continue;   // do not wait on a pid we may not have injected into
        }

        if (!wait_for_exit(pid)) break;   // asked to stop

        KITTY_LOGW("watch: %s (pid %d) exited - the payload went with it. "
                   "Waiting for init to restart it.", t::kProcName, pid);

        pid_t next = wait_for_respawn(pid);
        if (next <= 0) {
            if (g_stop) break;
            KITTY_LOGE("watch: %s did not come back within %ds.",
                       t::kProcName, kRespawnTimeoutMs / 1000);
            return 1;
        }
        KITTY_LOGI("watch: %s is back as pid %d.", t::kProcName, next);
    }

    KITTY_LOGI("watch: stopping. The payload stays loaded in the current "
               "%s; run recam_stop to remove it.", t::kProcName);
    return 0;
}
