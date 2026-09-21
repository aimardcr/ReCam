// ReCam injector - loads librecam.so into cameraserver.

#include <sys/ptrace.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/syscall.h>
#include <signal.h>
#include <unistd.h>
#include <dirent.h>
#include <string.h>
#include <errno.h>
#include <limits.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include <KittyMemoryMgr.hpp>
#include "Injector/KittyInjector.hpp"
#include "sepolicy.hpp"
#include "elfsym.hpp"
#include "target.hpp"
#include "../common/control.hpp"

namespace tgt = recam::target;

namespace sp = recam::sepolicy;

static const char* kTargetProc  = "cameraserver";
static const char* kInitSymbol  = "recam_init";
static const char* kPayloadName = "librecam.so";

// Remote-call timeout in ms. A remote call that never returns would hang cameraserver.
static const int kRemoteCallTimeoutMs = 3000;

// --------------------------------------------------------------------------- discovery

static bool file_exists(const std::string& p)
{
    struct stat st{};
    return ::stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

// Beside this binary first, since that is how it is deployed.
static std::string find_payload()
{
    char exe[PATH_MAX] = {0};
    ssize_t n = ::readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n > 0) {
        std::string dir(exe, static_cast<size_t>(n));
        size_t slash = dir.rfind('/');
        if (slash != std::string::npos) {
            std::string side = dir.substr(0, slash + 1) + kPayloadName;
            if (file_exists(side)) return side;
        }
    }

    const char* fallbacks[] = {
        "/data/local/tmp/",
        "/data/adb/recam/",
        "/data/adb/modules/recam/",
    };
    for (const char* d : fallbacks) {
        std::string p = std::string(d) + kPayloadName;
        if (file_exists(p)) return p;
    }
    return {};
}

// --------------------------------------------------------------------------- target

// Modules in the target that could hold the camera service code, best first.
static std::vector<std::pair<std::string, uintptr_t>> candidate_modules(pid_t pid)
{
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/maps", pid);
    FILE* f = fopen(path, "r");
    if (!f) return {};

    // Lowest mapped address per file, i.e. where file offset 0 landed.
    std::vector<std::pair<std::string, uintptr_t>> libs, exes;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        unsigned long start = 0, end = 0, off = 0;
        char perms[8] = {0}, file[384] = {0};
        if (sscanf(line, "%lx-%lx %7s %lx %*s %*s %383s",
                   &start, &end, perms, &off, file) != 5)
            continue;
        if (file[0] != '/' || off != 0) continue;

        const char* base = strrchr(file, '/');
        base = base ? base + 1 : file;

        auto already = [](std::vector<std::pair<std::string, uintptr_t>>& v,
                          const char* p) {
            for (auto& e : v) if (e.first == p) return true;
            return false;
        };
        if (strstr(base, "libcameraservice")) {
            if (!already(libs, file)) libs.emplace_back(file, start);
        } else if (strstr(base, "cameraserver")) {
            if (!already(exes, file)) exes.emplace_back(file, start);
        }
    }
    fclose(f);

    libs.insert(libs.end(), exes.begin(), exes.end());
    return libs;
}

// Resolve the hook target and confirm it against the target's live memory.
static uintptr_t resolve_hook_target(KittyMemoryMgr& kmgr, pid_t pid)
{
    for (auto& [modPath, mapBase] : candidate_modules(pid)) {
        recam::elfsym::Target t;
        if (!recam::elfsym::resolve_in_file(modPath, &t)) {
            KITTY_LOGI("target: not in %s", modPath.c_str());
            continue;
        }

        // The PT_LOAD covering file offset 0 does not always have p_vaddr 0.
        uint64_t firstVaddr = 0;
        if (!recam::elfsym::first_load_vaddr(modPath, &firstVaddr)) {
            KITTY_LOGW("target: no PT_LOAD in %s", modPath.c_str());
            continue;
        }
        const uintptr_t bias = mapBase - static_cast<uintptr_t>(firstVaddr);
        const uintptr_t runtime = bias + static_cast<uintptr_t>(t.vaddr);

        KITTY_LOGI("target: %s", modPath.c_str());
        KITTY_LOGI("target: found via %s, abi %s, %s", t.tier.c_str(), t.abi.c_str(),
                   t.is64 ? "ELF64" : "ELF32");
        KITTY_LOGI("target: vaddr 0x%llx  size %llu  bias %p  runtime %p",
                   (unsigned long long)t.vaddr, (unsigned long long)t.size,
                   reinterpret_cast<void*>(bias), reinterpret_cast<void*>(runtime));

        // A wrong offset would patch arbitrary code in a system daemon.
        uint8_t live[16] = {};
        if (!kmgr.readMem(runtime, live, sizeof(live))) {
            KITTY_LOGE("target: cannot read %p from the target.",
                       reinterpret_cast<void*>(runtime));
            continue;
        }
        if (memcmp(live, t.prologue, sizeof(live)) != 0) {
            char a[64] = {0}, b[64] = {0};
            for (int i = 0; i < 8; i++) {
                snprintf(a + i * 3, 4, "%02x ", t.prologue[i]);
                snprintf(b + i * 3, 4, "%02x ", live[i]);
            }
            KITTY_LOGE("target: prologue MISMATCH - file [%s] vs memory [%s]. "
                       "Refusing to hook.", a, b);
            continue;
        }

        char hex[64] = {0};
        for (int i = 0; i < 8; i++) snprintf(hex + i * 3, 4, "%02x ", live[i]);
        KITTY_LOGI("target: prologue verified against /proc/%d/mem [%s]", pid, hex);
        return runtime;
    }

    KITTY_LOGE("target: could not resolve %s in any candidate module.",
               recam::elfsym::kTargetPrefix);
    return 0;
}

// --------------------------------------------------------------------------- sepolicy

// How the payload will be handed to the target's dynamic linker.
enum class Route { None, Memfd, FilePath };

// /data/local is system_data_file, which cameraserver can search; /data/local/tmp is not.
static const char* kStageDir   = "/data/local/recam";
static const char* kStageLabel = "u:object_r:system_lib_file:s0";

// Copy the payload somewhere the target can actually dlopen by path, and relabel it.
static std::string stage_for_path_dlopen(const std::string& tgtCtx,
                                         const std::string& payload)
{
    if (!sp::has_permission(tgtCtx, kStageLabel, "file", "execute") ||
        !sp::has_permission(tgtCtx, kStageLabel, "file", "read") ||
        !sp::has_permission(tgtCtx, kStageLabel, "dir",  "search")) {
        KITTY_LOGI("route: target cannot execute %s either; file-path route unusable.",
                   kStageLabel);
        return {};
    }

    const std::string dst = std::string(kStageDir) + "/" + kPayloadName;

    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
             "mkdir -p '%s' && cp -f '%s' '%s' && chmod 644 '%s'",
             kStageDir, payload.c_str(), dst.c_str(), dst.c_str());
    if (::system(cmd) != 0) {
        KITTY_LOGW("route: could not stage the payload in %s", kStageDir);
        return {};
    }

    if (!sp::relabel(kStageDir, kStageLabel) || !sp::relabel(dst, kStageLabel)) {
        KITTY_LOGW("route: could not relabel %s to %s", kStageDir, kStageLabel);
        return {};
    }

    KITTY_LOGI("route: staged at %s, relabelled %s", dst.c_str(), kStageLabel);
    return dst;
}

// Work out how to get the payload loaded, patching policy if a tool is available.
static Route choose_route(pid_t targetPid, const std::string& payload,
                          std::string* stagedPath)
{
    if (!sp::available()) {
        KITTY_LOGI("sepolicy: selinuxfs not present, nothing to check.");
        return Route::Memfd;
    }
    if (!sp::enforcing()) {
        KITTY_LOGI("sepolicy: SELinux is globally permissive, nothing to do.");
        return Route::Memfd;
    }

    std::string selfCtx = sp::process_context(getpid());
    std::string tgtCtx  = sp::process_context(targetPid);
    std::string selfTy  = sp::context_type(selfCtx);
    std::string tgtTy   = sp::context_type(tgtCtx);

    if (selfTy.empty() || tgtTy.empty()) {
        KITTY_LOGW("sepolicy: could not read process contexts (self=\"%s\" target=\"%s\"); "
                   "skipping checks.", selfCtx.c_str(), tgtCtx.c_str());
        return Route::Memfd;
    }

    KITTY_LOGI("sepolicy: injector=%s  target=%s", selfCtx.c_str(), tgtCtx.c_str());

    // AOSP's tmpfs_domain() macro names these <domain>_tmpfs.
    const std::string tgtTmpfs = "u:object_r:" + tgtTy + "_tmpfs:s0";

    struct Need {
        std::string scon, tcon;
        const char* cls;
        const char* perm;
        std::string statement;
        std::string reason;
    };

    const std::vector<Need> needs = {
        { tgtCtx, tgtTmpfs, "file", "execute",
          "allow " + tgtTy + " " + tgtTy + "_tmpfs file execute",
          "linker must map the memfd-backed payload's executable segment" },

        { selfCtx, tgtCtx, "process", "ptrace",
          "allow " + selfTy + " " + tgtTy + " process ptrace",
          "PTRACE_SEIZE and /proc/pid/mem both route through ptrace_may_access()" },
    };

    std::vector<sp::Rule> missing;
    for (const auto& n : needs) {
        if (sp::has_permission(n.scon, n.tcon, n.cls, n.perm)) {
            KITTY_LOGI("sepolicy: ok   %s:%s { %s }", n.tcon.c_str(), n.cls, n.perm);
        } else {
            KITTY_LOGW("sepolicy: MISSING %s:%s { %s }", n.tcon.c_str(), n.cls, n.perm);
            missing.push_back({ n.statement, n.reason });
        }
    }

    if (missing.empty()) {
        KITTY_LOGI("sepolicy: all required permissions already granted.");
        return Route::Memfd;
    }

    std::string tool;
    sp::apply_live(missing, &tool);

    // Trust the kernel AVC, not the patch tool's exit code.
    bool allOk = true;
    for (const auto& n : needs) {
        bool ok = sp::has_permission(n.scon, n.tcon, n.cls, n.perm);
        KITTY_LOGI("sepolicy: verify %s:%s { %s } -> %s",
                   n.tcon.c_str(), n.cls, n.perm, ok ? "GRANTED" : "STILL DENIED");
        if (!ok) allOk = false;
    }

    if (allOk) {
        KITTY_LOGI("sepolicy: live policy patched%s%s. Reverts on reboot.",
                   tool.empty() ? "" : " via ", tool.c_str());
        return Route::Memfd;
    }

    // Fall back to a route that needs no policy change at all.
    KITTY_LOGI("route: memfd unavailable; trying the relabelled file-path route.");
    *stagedPath = stage_for_path_dlopen(tgtCtx, payload);
    if (!stagedPath->empty()) return Route::FilePath;

    return Route::None;
}

// --------------------------------------------------------------------------- main

int main()
{
    if (getuid() != 0) {
        KITTY_LOGE("Must run as root.");
        return 1;
    }

    std::string libPath = find_payload();
    if (libPath.empty()) {
        KITTY_LOGE("Could not find %s next to this binary, in /data/local/tmp "
                   "or /data/adb/recam.", kPayloadName);
        return 1;
    }
    KITTY_LOGI("Payload: %s", libPath.c_str());

    const pid_t pid = tgt::find(kTargetProc);
    if (pid <= 0) {
        KITTY_LOGE("Could not find process \"%s\".", kTargetProc);
        return 1;
    }

    const int sdk = tgt::sdk_level();
    KITTY_LOGI("Target: %s (pid %d)  sdk %d", kTargetProc, pid, sdk);

    // If a payload is already live, it must be stood down before a new one loads.
    const uintptr_t ctrlAddr = tgt::find_control_block(pid);
    if (ctrlAddr)
        KITTY_LOGI("A payload is already loaded (control block at %p); it will be "
                   "stood down before the new one loads.", reinterpret_cast<void*>(ctrlAddr));

    // An arm64-v8a payload cannot enter a 32-bit process; some builds run one.
    const bool localIs64  = !KittyMemoryEx::getMaps(getpid(), EProcMapFilter::Contains, "/lib64/").empty();
    const bool remoteIs64 = !KittyMemoryEx::getMaps(pid, EProcMapFilter::Contains, "/lib64/").empty();
    if (localIs64 != remoteIs64) {
        KITTY_LOGE("Injector is %d-bit but %s is %d-bit. Cannot inject.",
                   localIs64 ? 64 : 32, kTargetProc, remoteIs64 ? 64 : 32);
        return 1;
    }

    // Before attaching, so the target is never left stopped during a patch.
    std::string stagedPath;
    const Route route = choose_route(pid, libPath, &stagedPath);
    if (route == Route::None) {
        KITTY_LOGE("No usable way to load the payload into %s: memfd needs a policy "
                   "change this device has no tool to make, and the file-path route is "
                   "denied too. Refusing to inject (it would fail and disturb %s for "
                   "nothing).", kTargetProc, kTargetProc);
        return 1;
    }
    if (route == Route::FilePath) libPath = stagedPath;

    inject_elf_config_t cfg;
    cfg.sdk     = sdk;
    cfg.memfd   = (route == Route::Memfd);
    cfg.timeout = kRemoteCallTimeoutMs;

    // KittyInjector::init() probes memfd; probing twice perturbs its result.

    KittyMemoryMgr kmgr{};
    bool seized = false;
    if (!tgt::attach(kmgr, pid, sdk, cfg.timeout, &seized))
        return 1;
    cfg.seize = seized;

    KittyInjector injector{};
    if (!kmgr.initialize(pid, EK_MEM_OP_SYSCALL, true) || !injector.init(&kmgr, cfg)) {
        KITTY_LOGE("Injector init failed.");
        tgt::release(kmgr, pid);
        return 1;
    }

    // Exactly one payload is ever active.
    if (ctrlAddr && !tgt::shutdown_payload(kmgr, ctrlAddr)) {
        KITTY_LOGE("Could not stand down the existing payload; refusing to inject.");
        tgt::release(kmgr, pid);
        return 1;
    }

    // ELF parsing and LZMA do not belong inside a camera daemon.
    const uintptr_t hookTarget = resolve_hook_target(kmgr, pid);
    if (!hookTarget) {
        KITTY_LOGE("Could not resolve the hook target; refusing to inject.");
        tgt::release(kmgr, pid);
        return 1;
    }

    bool emulate = false;
    if (!injector.validateElf(libPath, nullptr, &emulate)) {
        KITTY_LOGE("Payload \"%s\" failed validation (wrong ABI?).", libPath.c_str());
        tgt::release(kmgr, pid);
        return 1;
    }

    KITTY_LOGI("Injecting %s (%s)...", libPath.c_str(),
               route == Route::Memfd ? "memfd" : "relabelled file path");
    inject_elf_info_t info = injector.inject(libPath);
    if (!info.is_valid()) {
        KITTY_LOGE("Injection failed. Target NOT killed; resuming it.");
        tgt::release(kmgr, pid);
        return 1;
    }
    KITTY_LOGI("Injected. dl_handle=%p", reinterpret_cast<void*>(info.dl_handle));

    // ---- call our own entry point -------------------------------------------------
    int rc = 0;
    const uintptr_t pInit = info.elf.findSymbol(kInitSymbol);
    if (!pInit) {
        KITTY_LOGE("Could not resolve \"%s\" in the injected payload. "
                   "Is it exported with default visibility?", kInitSymbol);
        rc = 1;
    } else {
        KITTY_LOGI("Calling %s(%d, %p) at %p ...", kInitSymbol, kINJ_SECRET_KEY,
                   reinterpret_cast<void*>(hookTarget), reinterpret_cast<void*>(pInit));

        auto call = kmgr.trace.callFunction(pInit,
                                            static_cast<uintptr_t>(kINJ_SECRET_KEY),
                                            hookTarget);
        if (call.status != KT_RP_CALL_SUCCESS) {
            KITTY_LOGE("%s did not complete (status %d).", kInitSymbol,
                       static_cast<int>(call.status));
            rc = 1;
        } else {
            KITTY_LOGI("%s returned %ld.", kInitSymbol, static_cast<long>(call.result.val));
            if (call.result.val != 0) rc = 1;
        }
    }

    // ---- detach -------------------------------------------------------------------
    if (!kmgr.trace.waitSyscall())
        KITTY_LOGW("waitSyscall before detach failed; detaching anyway.");

    tgt::release(kmgr, pid);

    KITTY_LOGI(rc == 0 ? "Done." : "Done, with errors.");
    return rc;
}
