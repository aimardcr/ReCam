// recam_stop - stand the payload down and give the real camera back.

#include <unistd.h>

#include <cstdio>
#include <string>

#include <KittyMemoryMgr.hpp>
#include "target.hpp"
#include "../common/control.hpp"

namespace t = recam::target;

static const int kRemoteCallTimeoutMs = 3000;

int main()
{
    if (getuid() != 0) {
        KITTY_LOGE("Must run as root.");
        return 1;
    }

    const pid_t pid = t::find(t::kProcName);
    if (pid <= 0) {
        KITTY_LOGE("Could not find process \"%s\".", t::kProcName);
        return 1;
    }

    const uintptr_t ctrlAddr = t::find_control_block(pid);
    if (!ctrlAddr) {
        KITTY_LOGI("No payload is loaded in %s (pid %d). Nothing to do.",
                   t::kProcName, pid);
        return 0;
    }

    const int sdk = t::sdk_level();
    KITTY_LOGI("Target: %s (pid %d) sdk %d, control block at %p",
               t::kProcName, pid, sdk, reinterpret_cast<void*>(ctrlAddr));

    KittyMemoryMgr kmgr{};
    if (!t::attach(kmgr, pid, sdk, kRemoteCallTimeoutMs, nullptr))
        return 1;

    if (!kmgr.initialize(pid, EK_MEM_OP_SYSCALL, true)) {
        KITTY_LOGE("Could not initialise memory access.");
        t::release(kmgr, pid);
        return 1;
    }

    const bool ok = t::shutdown_payload(kmgr, ctrlAddr);

    if (!kmgr.trace.waitSyscall())
        KITTY_LOGW("waitSyscall before detach failed; detaching anyway.");

    t::release(kmgr, pid);

    if (ok)
        KITTY_LOGI("Done. The real camera is restored; run recam_inject to hook again.");
    else
        KITTY_LOGE("Could not stand the payload down. "
                   "Kill %s (pid %d) to force a clean restart.", t::kProcName, pid);

    return ok ? 0 : 1;
}
