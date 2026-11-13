#include "sepolicy.hpp"

#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/xattr.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <climits>
#include <algorithm>
#include <cctype>

#include <KittyUtils.hpp>

namespace recam::sepolicy {

static const char* kSelinuxFs = "/sys/fs/selinux";

// --------------------------------------------------------------------------- basics

static std::string read_small_file(const std::string& path, size_t cap = 4096)
{
    int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return {};

    std::string out;
    out.resize(cap);
    ssize_t n = ::read(fd, out.data(), cap);
    ::close(fd);
    if (n <= 0) return {};

    out.resize(static_cast<size_t>(n));
    // These files are variously NUL- and newline-terminated.
    while (!out.empty() && (out.back() == '\n' || out.back() == '\0' || out.back() == ' '))
        out.pop_back();
    return out;
}

bool available()
{
    struct stat st{};
    return ::stat((std::string(kSelinuxFs) + "/access").c_str(), &st) == 0;
}

bool enforcing()
{
    return read_small_file(std::string(kSelinuxFs) + "/enforce") == "1";
}

std::string process_context(pid_t pid)
{
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/attr/current", pid);
    return read_small_file(path, 512);
}

std::string context_type(const std::string& context)
{
    // user:role:type:level  -> type
    size_t a = context.find(':');
    if (a == std::string::npos) return {};
    size_t b = context.find(':', a + 1);
    if (b == std::string::npos) return {};
    size_t c = context.find(':', b + 1);
    if (c == std::string::npos) return context.substr(b + 1);
    return context.substr(b + 1, c - b - 1);
}

static int class_index(const char* cls)
{
    std::string s = read_small_file(std::string(kSelinuxFs) + "/class/" + cls + "/index");
    if (s.empty()) return -1;
    return atoi(s.c_str());
}

static uint32_t perm_bit(const char* cls, const char* perm)
{
    std::string s = read_small_file(std::string(kSelinuxFs) + "/class/" + cls + "/perms/" + perm);
    if (s.empty()) return 0;
    int idx = atoi(s.c_str());           // selinuxfs reports a 1-based index
    if (idx < 1 || idx > 32) return 0;
    return 1u << (idx - 1);
}

// --------------------------------------------------------------------------- AVC query

namespace {

// Result of a kernel access-vector query.
struct Avc {
    bool     ok         = false;
    uint32_t allowed    = 0;
    bool     permissive = false;   // AVD_FLAGS_PERMISSIVE
};

Avc query(const std::string& scon, const std::string& tcon, const char* cls)
{
    Avc r;

    int clsNum = class_index(cls);
    if (clsNum < 0) return r;

    // Transaction file: the reply is readable only from the fd that wrote it.
    int fd = ::open((std::string(kSelinuxFs) + "/access").c_str(), O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        KITTY_LOGW("sepolicy: cannot open access transaction: %s", strerror(errno));
        return r;
    }

    char req[512];
    int n = snprintf(req, sizeof(req), "%s %s %d", scon.c_str(), tcon.c_str(), clsNum);
    if (n <= 0 || n >= static_cast<int>(sizeof(req))) {
        ::close(fd);
        return r;
    }
    // selinuxfs accepts one complete request per descriptor, not partial writes.
    if (::write(fd, req, static_cast<size_t>(n)) != n) {
        KITTY_LOGW("sepolicy: access query failed for %s -> %s:%s: %s",
                   scon.c_str(), tcon.c_str(), cls, strerror(errno));
        ::close(fd);
        return r;
    }

    char rep[256] = {0};
    ssize_t got = ::read(fd, rep, sizeof(rep) - 1);
    ::close(fd);
    if (got <= 0) return r;

    // "allowed decided auditallow auditdeny seqno flags", all hex except seqno.
    unsigned int allowed = 0, decided = 0, auditallow = 0, auditdeny = 0, seqno = 0, flags = 0;
    if (sscanf(rep, "%x %x %x %x %u %x",
               &allowed, &decided, &auditallow, &auditdeny, &seqno, &flags) < 6)
        return r;

    r.ok         = true;
    r.allowed    = allowed;
    r.permissive = (flags & 0x1) != 0;   // AVD_FLAGS_PERMISSIVE
    return r;
}

}  // namespace

Access permission(const std::string& scon, const std::string& tcon,
                  const char* cls, const char* perm)
{
    uint32_t bit = perm_bit(cls, perm);
    if (!bit) return Access::Unknown;
    Avc a = query(scon, tcon, cls);
    if (!a.ok) return Access::Unknown;
    return a.permissive || (a.allowed & bit) ? Access::Allowed : Access::Denied;
}

bool has_permission(const std::string& scon, const std::string& tcon,
                    const char* cls, const char* perm)
{
    return permission(scon, tcon, cls, perm) == Access::Allowed;
}

// --------------------------------------------------------------------------- patching

namespace {

struct PatchTool {
    std::string path;
    const char* argfmt;   // %s is replaced by the quoted rule statement
};

// Live policy only; none of these survives a reboot.
const PatchTool kTools[] = {
    { "/data/adb/ksud",                 "sepolicy patch '%s'" },   // KernelSU
    { "/data/adb/ksu/bin/ksud",         "sepolicy patch '%s'" },
    { "/data/adb/magisk/magiskpolicy",  "--live '%s'" },           // Magisk
    { "/system/bin/magiskpolicy",       "--live '%s'" },
    { "/sbin/magiskpolicy",             "--live '%s'" },
    { "/debug_ramdisk/magiskpolicy",    "--live '%s'" },
    { "/system/bin/supolicy",           "--live '%s'" },           // SuperSU
};

bool exists(const char* p)
{
    struct stat st{};
    return ::stat(p, &st) == 0 && S_ISREG(st.st_mode) && ::access(p, X_OK) == 0;
}

bool run(const std::string& cmd)
{
    int rc = ::system(cmd.c_str());
    return rc == 0;
}

std::string quote(const std::string& value)
{
    std::string out = "'";
    for (char c : value) out += c == '\'' ? "'\\''" : std::string(1, c);
    return out + "'";
}

std::string on_path(const char* name)
{
    const char* env = getenv("PATH");
    if (!env) return {};
    std::string path(env);
    size_t start = 0;
    do {
        const auto end = path.find(':', start);
        const auto dir = path.substr(start, end - start);
        if (!dir.empty() && dir[0] == '/') {
            const auto candidate = dir + "/" + name;
            if (exists(candidate.c_str())) return candidate;
        }
        if (end == std::string::npos) break;
        start = end + 1;
    } while (start < path.size());
    return {};
}

std::vector<PatchTool> patch_tools()
{
    std::vector<PatchTool> tools;
    for (const auto& tool : kTools)
        if (exists(tool.path.c_str())) tools.push_back(tool);
    for (const char* name : {"magiskpolicy", "supolicy"}) {
        const auto path = on_path(name);
        if (!path.empty() && std::none_of(tools.begin(), tools.end(),
            [&](const PatchTool& tool) { return tool.path == path; }))
            tools.push_back({path, "--live '%s'"});
    }
    if (!tools.empty()) return tools;
    // Magisk's tmpfs location varies by boot layout; ask its installed executable.
    const auto magisk = on_path("magisk");
    if (magisk.empty()) return tools;
    FILE* pipe = popen((quote(magisk) + " --path 2>/dev/null").c_str(), "r");
    if (!pipe) return tools;
    char buf[PATH_MAX]{};
    const bool got = fgets(buf, sizeof(buf), pipe) != nullptr;
    const int rc = pclose(pipe);
    if (!got || rc != 0 || buf[0] != '/') return tools;
    std::string dir(buf);
    while (!dir.empty() && (dir.back() == '\n' || dir.back() == '\r')) dir.pop_back();
    const auto policy = dir + "/magiskpolicy";
    if (exists(policy.c_str())) tools.push_back({policy, "--live '%s'"});
    return tools;
}

}  // namespace

bool relabel(const std::string& path, const char* context)
{
    // chcon is part of toybox on every Android build this targets.
    std::string cmd = "chcon " + std::string(context) + " '" + path + "' >/dev/null 2>&1";
    if (!run(cmd)) return false;

    // Read the label back rather than trusting chcon's exit code.
    char buf[256] = {0};
    ssize_t n = ::lgetxattr(path.c_str(), "security.selinux", buf, sizeof(buf) - 1);
    if (n <= 0) return false;
    while (n > 0 && (buf[n - 1] == 0 || buf[n - 1] == 10)) n--;   // strip NUL / newline
    return std::string(buf, static_cast<size_t>(n)) == context;
}

bool apply_live(const std::vector<Rule>& rules, std::string* toolUsed)
{
    if (rules.empty()) return true;

    for (const auto& tool : patch_tools()) {

        KITTY_LOGI("sepolicy: using %s", tool.path.c_str());

        bool all = true;
        for (const auto& r : rules) {
            char args[768];
            snprintf(args, sizeof(args), tool.argfmt, r.statement.c_str());

            std::string cmd = quote(tool.path) + " " + args;
            KITTY_LOGI("sepolicy:   %s", r.statement.c_str());
            KITTY_LOGI("sepolicy:     (%s)", r.reason.c_str());

            if (!run(cmd)) {
                KITTY_LOGW("sepolicy:   tool reported failure for this rule");
                all = false;
            }
        }

        // Report the tool even on partial failure; the caller re-checks the AVC.
        if (toolUsed) *toolUsed = tool.path;
        if (all) return true;
    }

    KITTY_LOGE("sepolicy: no policy patch tool succeeded "
               "(looked for ksud, magiskpolicy, supolicy).");
    return false;
}

void prepare_c2d(pid_t targetPid)
{
    if (!available() || !enforcing()) return;
    const auto source = process_context(targetPid);
    if (context_type(source) != "cameraserver") return;
    const char* paths[] = {"/vendor/lib64/libC2D2.so", "/vendor/lib64/libc2d30_bltlib.so",
                           "/vendor/lib64/libqdMetaData.so"};
    std::vector<std::string> contexts;
    for (const char* path : paths) {
        const auto targetPath = "/proc/" + std::to_string(targetPid) + "/root" + path;
        struct stat st{};
        if (stat(targetPath.c_str(), &st) || !S_ISREG(st.st_mode)) continue;
        char label[512]{};
        const ssize_t size = lgetxattr(targetPath.c_str(), "security.selinux", label, sizeof(label) - 1);
        if (size <= 0) {
            KITTY_LOGW("C2D: cannot read label of %s: %s", path, strerror(errno));
            continue;
        }
        const std::string context(label);
        const auto type = context_type(context);
        if (type.empty() || !std::all_of(type.begin(), type.end(),
            [](unsigned char c) { return std::isalnum(c) || c == '_'; })) continue;
        if (std::find(contexts.begin(), contexts.end(), context) == contexts.end())
            contexts.push_back(context);
    }
    const char* perms[] = {"open", "read", "getattr", "map", "execute"};
    std::vector<Rule> missing;
    for (const auto& context : contexts) {
        for (const char* perm : perms) {
            const auto decision = permission(source, context, "file", perm);
            if (decision == Access::Unknown) {
                KITTY_LOGW("C2D: policy query unavailable for %s:file %s; no rule inferred",
                           context.c_str(), perm);
                continue;
            }
            if (decision == Access::Allowed) continue;
            missing.push_back({"allow cameraserver " + context_type(context) + " file " + perm,
                               "load installed Qualcomm C2D libraries in cameraserver"});
        }
    }
    if (missing.empty()) return;
    std::string tool;
    apply_live(missing, &tool);
    bool ready = true;
    for (const auto& context : contexts) {
        for (const char* perm : perms) {
            const auto result = permission(source, context, "file", perm);
            if (result == Access::Allowed) continue;
            ready = false;
            KITTY_LOGW("C2D: %s:file %s remains %s", context.c_str(), perm,
                       result == Access::Denied ? "denied" : "unverified");
        }
    }
    KITTY_LOGI("C2D: library access %s%s%s", ready ? "granted" : "unavailable",
               tool.empty() ? "" : " via ", tool.c_str());
}

}  // namespace recam::sepolicy
