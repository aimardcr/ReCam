// ReCam payload - loads into cameraserver and hooks the frame-return path.

#include <android/log.h>
#include <dlfcn.h>
#include <link.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <unistd.h>

#include "../common/control.hpp"
#include "framehook.hpp"
#include "shmsource.hpp"

#define LOG_TAG "ReCam"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

#define EXPORT __attribute__((visibility("default")))

// Must match kINJ_SECRET_KEY in AndKittyInjector/src/Injector/KittyInjector.hpp.
static const uintptr_t kInjectorKey = 1337;

using recam::ControlBlock;

static ControlBlock* g_ctrl = nullptr;   // shared across copies; see control.hpp
static bool          g_owner = false;    // true if THIS copy currently owns the process

#ifndef PR_SET_VMA
#define PR_SET_VMA           0x53564d41
#define PR_SET_VMA_ANON_NAME 0
#endif

// --------------------------------------------------------------------------- control block

// A previous copy's block is found by its PR_SET_VMA name, never by a path.
static ControlBlock* find_control_block()
{
    FILE* f = fopen("/proc/self/maps", "r");
    if (!f) return nullptr;

    ControlBlock* found = nullptr;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        if (!strstr(line, RECAM_MARKER_MAPS)) continue;

        unsigned long start = 0, end = 0;
        if (sscanf(line, "%lx-%lx", &start, &end) != 2) continue;
        if (end - start < sizeof(ControlBlock)) continue;

        ControlBlock* c = reinterpret_cast<ControlBlock*>(start);
        // Validate before trusting a page we found by name alone.
        if (c->magic == recam::kControlMagic && c->version == recam::kControlVersion) {
            found = c;
            break;
        }
        LOGW("control: mapping at %lx is named like ours but has magic %llx - ignoring",
             start, static_cast<unsigned long long>(c->magic));
    }
    fclose(f);
    return found;
}

// Create the control block in a one-page anonymous mapping named "[anon:recam]".
static ControlBlock* create_control_block()
{
    const size_t len = 4096;
    void* p = mmap(nullptr, len, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        LOGE("control: mmap failed");
        return nullptr;
    }

    // Unnamed, the injector could not find this block to stand it down.
    if (prctl(PR_SET_VMA, PR_SET_VMA_ANON_NAME, p, len, RECAM_MARKER_NAME) != 0) {
        LOGE("control: PR_SET_VMA failed - the injector will not be able to detect "
             "or stand down this copy. Refusing to take ownership.");
        munmap(p, len);
        return nullptr;
    }

    ControlBlock* c = static_cast<ControlBlock*>(p);
    memset(c, 0, sizeof(*c));
    c->magic   = recam::kControlMagic;
    c->version = recam::kControlVersion;
    return c;   // deliberately never unmapped - it outlives every payload copy
}

// Load bias of this library, for the log line and for sanity checks.
static uintptr_t self_base()
{
    Dl_info info{};
    if (dladdr(reinterpret_cast<void*>(&self_base), &info) && info.dli_fbase)
        return reinterpret_cast<uintptr_t>(info.dli_fbase);
    return 0;
}

// --------------------------------------------------------------------------- hooks

static void remove_all_hooks()
{
    if (g_ctrl && g_ctrl->hooks_installed == 0) {
        LOGI("shutdown: no hooks to remove");
        return;
    }
    if (recam::framehook_remove() && g_ctrl)
        g_ctrl->hooks_installed = 0;
}

// --------------------------------------------------------------------------- module scan

// Version-independent: everything after it is the release-specific parameter list.
static const char kTargetPrefix[] =
    "_ZN7android7camera319Camera3OutputStream25returnBufferCheckedLocked";

struct ScanResult {
    const char* path;
    uintptr_t   bias;
    bool        found;
};

// Does this module's basename look like it could host the camera service code?
static bool is_candidate(const char* path)
{
    if (!path || !*path) return false;
    const char* base = strrchr(path, '/');
    base = base ? base + 1 : path;
    return strstr(base, "libcameraservice") != nullptr ||
           strstr(base, "cameraserver") != nullptr;
}

static int phdr_cb(struct dl_phdr_info* info, size_t, void* data)
{
    ScanResult* out = static_cast<ScanResult*>(data);

    // dl_iterate_phdr reports the main executable with an empty name.
    const char* path = (info->dlpi_name && *info->dlpi_name) ? info->dlpi_name : "<exe>";

    if (!is_candidate(path) && strcmp(path, "<exe>") != 0)
        return 0;

    // Prefer a real libcameraservice.so over the executable if both are present.
    if (out->found && strstr(path, "libcameraservice") == nullptr)
        return 0;

    out->path  = path;
    out->bias  = static_cast<uintptr_t>(info->dlpi_addr);
    out->found = true;

    // Keep scanning: a libcameraservice.so seen later should win over <exe>.
    return 0;
}

// Logs which module holds the camera service code, as seen from *inside* the process.
static void log_target_module()
{
    ScanResult r = { nullptr, 0, false };
    dl_iterate_phdr(phdr_cb, &r);

    if (!r.found) {
        LOGW("module scan: no libcameraservice.so and no cameraserver mapping found");
        return;
    }
    LOGI("module scan: candidate '%s' load bias %p", r.path, reinterpret_cast<void*>(r.bias));

    // Exported on Android 11-14, where libcameraservice.so is a shared library.
    void* sym = dlsym(RTLD_DEFAULT, kTargetPrefix);
    if (sym)
        LOGI("module scan: target exported, dlsym -> %p", sym);
    else
        LOGI("module scan: dlsym(prefix) found nothing - expected either way, since "
             "dlsym needs a full mangled name and the suffix is release-specific. "
             "The injector supplies the resolved address.");
}

// --------------------------------------------------------------------------- entry points

// The kill switch, called remotely by the injector on the previous copy.
extern "C" EXPORT int recam_shutdown(uintptr_t key)
{
    if (key != kInjectorKey) {
        LOGW("recam_shutdown: bad key %p, ignoring", reinterpret_cast<void*>(key));
        return -1;
    }

    if (!g_owner) {
        LOGI("recam_shutdown: this copy does not own the process; nothing to do");
        return 0;
    }

    LOGI("=== ReCam shutdown (generation %u, base %p) ===",
         g_ctrl ? g_ctrl->generation : 0, reinterpret_cast<void*>(self_base()));

    remove_all_hooks();

    g_owner = false;
    if (g_ctrl) {
        g_ctrl->active      = 0;
        g_ctrl->shutdown_fn = 0;
        g_ctrl->owner_base  = 0;
    }

    // The image stays mapped; dlclose on an injected library is never safe here.
    LOGI("shutdown: hooks removed, this copy is now dormant");
    return 0;
}

// key        must be kInjectorKey; proves the call came from our injector.
extern "C" EXPORT int recam_init(uintptr_t key, uintptr_t hookTarget)
{
    if (key != kInjectorKey) {
        LOGW("recam_init: bad key %p, ignoring", reinterpret_cast<void*>(key));
        return -1;
    }
    if (g_owner) {
        LOGW("recam_init: this copy is already the owner, ignoring");
        return 0;
    }

    // Reusing a previous copy's block is what makes two owners impossible.
    ControlBlock* c = find_control_block();
    const bool reused = (c != nullptr);
    if (!c) c = create_control_block();
    if (!c) {
        LOGE("recam_init: no control block; refusing to take ownership");
        return -1;
    }

    // The injector should already have stood the previous owner down.
    if (c->active) {
        LOGE("recam_init: generation %u is still ACTIVE (shutdown_fn %p). "
             "Refusing to take over - restart cameraserver.",
             c->generation, reinterpret_cast<void*>(c->shutdown_fn));
        return -1;
    }

    g_ctrl = c;
    g_owner = true;

    c->generation++;
    c->owner_pid       = static_cast<uint32_t>(getpid());
    c->owner_base      = self_base();
    c->shutdown_fn     = reinterpret_cast<uint64_t>(&recam_shutdown);
    c->hooks_installed = 0;
    c->active          = 1;

    LOGI("=== ReCam payload init (generation %u) ===", c->generation);
    LOGI("pid %d  uid %d  base %p  control %p%s", getpid(), getuid(),
         reinterpret_cast<void*>(c->owner_base), reinterpret_cast<void*>(c),
         reused ? " (reused)" : " (new)");
    log_target_module();

    // Before hooking, so the very first frame can already be substituted.
    int      shmFd   = 0;
    uint64_t shmSize = 0;
    if (recam::shmsource_create(&shmFd, &shmSize)) {
        c->shm_fd   = static_cast<uint32_t>(shmFd);
        c->shm_size = shmSize;
        LOGI("shm: published fd %d (%llu bytes) - a feeder reaches it at "
             "/proc/%d/fd/%d", shmFd, static_cast<unsigned long long>(shmSize),
             getpid(), shmFd);
    } else {
        c->shm_fd = 0;
        c->shm_size = 0;
        LOGW("shm: no frame ring; only the built-in colour bars are available");
    }

    if (!hookTarget) {
        LOGW("recam_init: no hook target supplied; staying passive");
        return 0;
    }

    LOGI("hook target: %p", reinterpret_cast<void*>(hookTarget));
    if (recam::framehook_install(hookTarget)) {
        c->hooks_installed = 1;
    } else {
        // Not a failed init: the payload stays loaded and dormant.
        LOGE("recam_init: hook install failed; payload is loaded but inert");
    }

    LOGI("recam_init: done");
    return 0;
}

// Exercises the hook and pixel paths a live camera session never reaches.
extern "C" EXPORT int recam_selftest(uintptr_t key)
{
    if (key != kInjectorKey) {
        LOGW("recam_selftest: bad key, ignoring");
        return -1;
    }
    return recam::selftest_run();
}

// Only reachable if injected into a process that has a JVM. cameraserver does not.
extern "C" EXPORT int JNI_OnLoad(void* /*vm*/, void* key)
{
    LOGI("JNI_OnLoad called (key %p) - process has a JVM", key);
    if (reinterpret_cast<uintptr_t>(key) == kInjectorKey)
        recam_init(kInjectorKey, 0);
    return 0x00010006;  // JNI_VERSION_1_6
}

__attribute__((constructor)) static void recam_ctor()
{
    // The loader holds its own lock here, so do no work.
    LOGI("librecam.so constructor: loaded into pid %d", getpid());
}
