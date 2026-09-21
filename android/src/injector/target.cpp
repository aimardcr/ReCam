#include "target.hpp"

#include <sys/ptrace.h>
#include <signal.h>
#include <unistd.h>
#include <dirent.h>
#include <string.h>
#include <errno.h>

#include <cstdio>
#include <cstdlib>

#include "Injector/KittyInjector.hpp"
#include "../common/control.hpp"

namespace recam::target {

const char* const kProcName = "cameraserver";

int sdk_level()
{
    FILE* p = popen("getprop ro.build.version.sdk", "r");
    if (!p) return 0;
    char buf[32] = {0};
    if (!fgets(buf, sizeof(buf), p)) { pclose(p); return 0; }
    pclose(p);
    return atoi(buf);
}

std::string proc_name(pid_t pid)
{
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/cmdline", pid);
    FILE* f = fopen(path, "rb");
    if (!f) return {};
    char buf[256] = {0};
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    if (n == 0) return {};
    const char* base = strrchr(buf, '/');
    return base ? base + 1 : buf;
}

pid_t find(const char* name)
{
    DIR* d = opendir("/proc");
    if (!d) return -1;

    pid_t found = -1;
    struct dirent* e;
    while ((e = readdir(d)) != nullptr) {
        if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
        pid_t p = static_cast<pid_t>(atoi(e->d_name));
        if (proc_name(p) == name) { found = p; break; }
    }
    closedir(d);
    return found;
}

uintptr_t find_control_block(pid_t pid)
{
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/maps", pid);
    FILE* f = fopen(path, "r");
    if (!f) return 0;

    uintptr_t addr = 0;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        if (!strstr(line, RECAM_MARKER_MAPS)) continue;
        unsigned long start = 0, end = 0;
        if (sscanf(line, "%lx-%lx", &start, &end) == 2 &&
            end - start >= sizeof(recam::ControlBlock)) {
            addr = static_cast<uintptr_t>(start);
            break;
        }
    }
    fclose(f);
    return addr;
}

bool attach(KittyMemoryMgr& kmgr, pid_t pid, int sdk, int timeoutMs, bool* seizedOut)
{
    kmgr.trace = KittyTraceMgr(pid, 0, true, timeoutMs);

    // Stop every thread, then resume only the main thread so ptrace can take it.
    if (kill(pid, SIGSTOP) == -1) {
        KITTY_LOGE("SIGSTOP failed: %s", strerror(errno));
        return false;
    }
    if (tgkill(pid, pid, SIGCONT) == -1) {
        KITTY_LOGE("tgkill SIGCONT failed: %s", strerror(errno));
        kill(pid, SIGCONT);
        return false;
    }

    // EXITKILL: a half-injected daemon is worse than a restarted one.
    errno = 0;
    bool seized = (sdk >= 21) &&
                  kmgr.trace.seize(PTRACE_O_EXITKILL | PTRACE_O_TRACESYSGOOD);
    bool attached = seized ||
                    kmgr.trace.attach(PTRACE_O_EXITKILL | PTRACE_O_TRACESYSGOOD);

    if (!attached) {
        KITTY_LOGE("Failed to attach to %d. Is SELinux blocking process:ptrace?", pid);
        kill(pid, SIGCONT);
        return false;
    }
    if (seized && !kmgr.trace.stop()) {
        KITTY_LOGE("Failed to interrupt target.");
        release(kmgr, pid);
        return false;
    }

    if (seizedOut) *seizedOut = seized;
    KITTY_LOGI("Attached.");
    return true;
}

void release(KittyMemoryMgr& kmgr, pid_t pid)
{
    kmgr.trace.detach();
    // Non-main threads stay SIGSTOPped until this runs.
    if (kill(pid, SIGCONT) == -1)
        KITTY_LOGE("Failed to SIGCONT %d: %s", pid, strerror(errno));
    else
        KITTY_LOGI("Target %d resumed.", pid);
}

bool read_control(KittyMemoryMgr& kmgr, uintptr_t ctrlAddr, recam::ControlBlock* out)
{
    if (!ctrlAddr || !out) return false;

    if (!kmgr.readMem(ctrlAddr, out, sizeof(*out))) {
        KITTY_LOGE("control: could not read block at %p.",
                   reinterpret_cast<void*>(ctrlAddr));
        return false;
    }
    if (out->magic != recam::kControlMagic) {
        KITTY_LOGE("control: block at %p has bad magic 0x%llx.",
                   reinterpret_cast<void*>(ctrlAddr),
                   static_cast<unsigned long long>(out->magic));
        return false;
    }
    if (out->version != recam::kControlVersion) {
        KITTY_LOGE("control: block version %u, this tool speaks %u. "
                   "Restart cameraserver to clear it.",
                   out->version, recam::kControlVersion);
        return false;
    }
    return true;
}

bool shutdown_payload(KittyMemoryMgr& kmgr, uintptr_t ctrlAddr)
{
    recam::ControlBlock cb{};
    if (!read_control(kmgr, ctrlAddr, &cb)) return false;

    KITTY_LOGI("payload: generation %u, active=%u, hooks=%u, base=%p",
               cb.generation, cb.active, cb.hooks_installed,
               reinterpret_cast<void*>(cb.owner_base));

    if (!cb.active) {
        KITTY_LOGI("payload: already dormant, nothing to stand down.");
        return true;
    }
    if (!cb.shutdown_fn) {
        KITTY_LOGE("payload: active but records no shutdown function. "
                   "Restart cameraserver.");
        return false;
    }

    // Stopped under ptrace: the safest moment to remove an inline hook.
    KITTY_LOGI("payload: calling recam_shutdown at %p on generation %u ...",
               reinterpret_cast<void*>(cb.shutdown_fn), cb.generation);

    auto call = kmgr.trace.callFunction(static_cast<uintptr_t>(cb.shutdown_fn),
                                        static_cast<uintptr_t>(kINJ_SECRET_KEY));
    if (call.status != KT_RP_CALL_SUCCESS) {
        KITTY_LOGE("payload: recam_shutdown did not complete (status %d).",
                   static_cast<int>(call.status));
        return false;
    }
    if (call.result.val != 0) {
        KITTY_LOGE("payload: recam_shutdown returned %ld.",
                   static_cast<long>(call.result.val));
        return false;
    }

    // Confirm from the control block; two live payloads must be impossible.
    if (!read_control(kmgr, ctrlAddr, &cb)) return false;
    if (cb.active) {
        KITTY_LOGE("payload: still reports active after shutdown.");
        return false;
    }

    KITTY_LOGI("payload: generation %u is dormant (%u hooks installed). "
               "Its image stays mapped but does nothing.",
               cb.generation, cb.hooks_installed);
    return true;
}

}  // namespace recam::target
