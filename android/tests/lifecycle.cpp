// Standalone Android process: exercise the real payload lifecycle with failing
// hook operations, without injecting faults into cameraserver.
#include "../src/payload/payload.cpp"

#include <cstdlib>

static bool install_ok = true;
static bool prepare_ok = true;
static bool remove_ok = true;
static unsigned installs = 0;
static unsigned removals = 0;
static unsigned checks = 0;

namespace recam {
bool framehook_prepare(uintptr_t) { return prepare_ok; }
bool framehook_install(uintptr_t) { ++installs; return install_ok; }
bool framehook_remove() { ++removals; return remove_ok; }
bool shmsource_create(int*, uint64_t*) { return false; }
int selftest_run() { return 0; }
}

static void check(bool ok, const char* message)
{
    ++checks;
    if (!ok) {
        fprintf(stderr, "FAIL: %s\n", message);
        std::exit(1);
    }
}

int main()
{
    constexpr uintptr_t target = 0x1000; // consumed only by the hook stub
    check(recam_init(0, target) != 0, "reject invalid init key");
    check(recam_init(kInjectorKey, 0) != 0, "reject missing hook target");
    check(!g_owner && !g_ctrl && installs == 0, "invalid init cannot claim ownership");

    install_ok = false;
    check(recam_init(kInjectorKey, target) != 0, "propagate failed installation");
    check(removals == 1, "attempt cleanup even before hook count is published");
    check(g_ctrl && !g_owner && !g_ctrl->active && !g_ctrl->hooks_installed &&
          !g_ctrl->shutdown_fn && !g_ctrl->owner_base, "failed init leaves a dormant copy");

    install_ok = true;
    check(recam_init(kInjectorKey, target) == 0, "retry init after failure");
    check(g_owner && g_ctrl->active && g_ctrl->hooks_installed == 1 &&
          g_ctrl->shutdown_fn && g_ctrl->owner_base, "successful init owns the hook");
    g_ctrl->shm_fd = 42; g_ctrl->shm_size = 4096;
    ControlBlock before = *g_ctrl;
    const unsigned prior_installs = installs;
    check(recam_init(kInjectorKey, target) == 0 && installs == prior_installs,
          "repeated init does not install twice");

    remove_ok = false;
    check(recam_shutdown(kInjectorKey) != 0, "propagate failed removal");
    check(g_owner && memcmp(g_ctrl, &before, sizeof(before)) == 0,
          "failed removal preserves every control field and local ownership");
    check(recam_shutdown(0) != 0 && g_owner, "invalid shutdown key cannot clear ownership");

    // Simulate another loaded copy discovering the same process-wide block.
    g_owner = false;
    check(recam_init(kInjectorKey, target) != 0 && installs == prior_installs,
          "another copy cannot take over after failed shutdown");
    g_owner = true;

    remove_ok = true;
    check(recam_shutdown(kInjectorKey) == 0, "retry shutdown after removal failure");
    check(!g_owner && !g_ctrl->active && !g_ctrl->hooks_installed &&
          !g_ctrl->shutdown_fn && !g_ctrl->owner_base && !g_ctrl->shm_fd && !g_ctrl->shm_size,
          "successful shutdown clears ownership and ring handles");
    const unsigned prior_removals = removals;
    check(recam_shutdown(kInjectorKey) == 0 && removals == prior_removals,
          "repeated shutdown is harmless");

    const auto* resident = reinterpret_cast<const recam::ResidentPayload*>(
        reinterpret_cast<const uint8_t*>(g_ctrl) + recam::kResidentOffset);
    check(resident->magic == recam::kResidentMagic &&
          resident->init == reinterpret_cast<uintptr_t>(&recam_init) &&
          resident->generation == g_ctrl->generation,
          "stopped payload retains a matching resident identity");
    before = *g_ctrl;
    prepare_ok = false;
    check(recam_init(kInjectorKey, target) == -2 && !g_owner &&
          memcmp(g_ctrl, &before, sizeof(before)) == 0,
          "busy retirement returns retry without mutating ownership or generation");
    prepare_ok = true;

    install_ok = false;
    remove_ok = false;
    check(recam_init(kInjectorKey, target) != 0, "report init failure when cleanup also fails");
    check(g_owner && g_ctrl->active && g_ctrl->shutdown_fn && g_ctrl->owner_base,
          "failed cleanup preserves shutdown entry point");
    check(recam_init(kInjectorKey, target) != 0,
          "ownership without a working hook is not successful initialization");
    remove_ok = true;
    check(recam_shutdown(kInjectorKey) == 0 && !g_owner,
          "failed initialization can still be shut down later");

    // An older payload may have cleared active while leaving a hook behind.
    g_ctrl->hooks_installed = 1;
    check(recam_init(kInjectorKey, target) != 0,
          "inconsistent dormant state cannot be taken over");
    g_ctrl->hooks_installed = 0;

    printf("%u lifecycle checks passed\n", checks);
    return 0;
}
