// recam_test - run the payload's self-test inside cameraserver.

#include <unistd.h>

#include <cstdio>

#include <KittyMemoryMgr.hpp>
#include "Injector/KittyInjector.hpp"
#include "target.hpp"
#include "../common/control.hpp"

namespace t = recam::target;

static const int kRemoteCallTimeoutMs = 60000;  // the pixel tests touch a few MB

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
        KITTY_LOGE("No payload loaded in %s. Run recam_inject first.", t::kProcName);
        return 1;
    }

    KittyMemoryMgr kmgr{};
    if (!t::attach(kmgr, pid, t::sdk_level(), kRemoteCallTimeoutMs, nullptr))
        return 1;

    if (!kmgr.initialize(pid, EK_MEM_OP_SYSCALL, true)) {
        KITTY_LOGE("Could not initialise memory access.");
        t::release(kmgr, pid);
        return 1;
    }

    recam::ControlBlock cb{};
    if (!t::read_control(kmgr, ctrlAddr, &cb)) {
        t::release(kmgr, pid);
        return 1;
    }

    // Resolve from the image the control block names, since a memfd load has no path.
    ElfScanner payload = kmgr.elfScanner.createWithBase(
        static_cast<uintptr_t>(cb.owner_base));
    const uintptr_t fn = payload.findSymbol("recam_selftest");
    if (!fn) {
        KITTY_LOGE("Could not resolve recam_selftest in the payload at %p.",
                   reinterpret_cast<void*>(cb.owner_base));
        t::release(kmgr, pid);
        return 1;
    }

    KITTY_LOGI("Running self-test in %s (pid %d), generation %u ...",
               t::kProcName, pid, cb.generation);
    KITTY_LOGI("Watch results with: adb logcat -s ReCamTest");

    auto call = kmgr.trace.callFunction(fn, static_cast<uintptr_t>(kINJ_SECRET_KEY));

    int rc = 1;
    if (call.status != KT_RP_CALL_SUCCESS) {
        KITTY_LOGE("recam_selftest did not complete (status %d).",
                   static_cast<int>(call.status));
    } else {
        const long failures = static_cast<long>(call.result.val);
        if (failures == 0) { KITTY_LOGI("Self-test: ALL CHECKS PASSED."); rc = 0; }
        else               { KITTY_LOGE("Self-test: %ld check(s) FAILED.", failures); }
    }

    if (!kmgr.trace.waitSyscall())
        KITTY_LOGW("waitSyscall before detach failed; detaching anyway.");
    t::release(kmgr, pid);
    return rc;
}
