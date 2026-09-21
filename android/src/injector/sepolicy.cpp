#include "sepolicy.hpp"

#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/xattr.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

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
    if (fd < 0) return r;

    char req[512];
    int n = snprintf(req, sizeof(req), "%s %s %d", scon.c_str(), tcon.c_str(), clsNum);
    if (n <= 0 || ::write(fd, req, static_cast<size_t>(n)) != n) {
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

bool has_permission(const std::string& scon, const std::string& tcon,
                    const char* cls, const char* perm)
{
    uint32_t bit = perm_bit(cls, perm);
    if (!bit) return false;

    Avc a = query(scon, tcon, cls);
    if (!a.ok) return false;

    // A permissive source domain will not be denied anything, so treat it as granted.
    return a.permissive || (a.allowed & bit) != 0;
}

// --------------------------------------------------------------------------- patching

namespace {

struct PatchTool {
    const char* path;
    const char* argfmt;   // %s is replaced by the quoted rule statement
};

// Live policy only; none of these survives a reboot.
const PatchTool kTools[] = {
    { "/data/adb/ksud",                 "sepolicy patch '%s'" },   // KernelSU
    { "/data/adb/ksu/bin/ksud",         "sepolicy patch '%s'" },
    { "/data/adb/magisk/magiskpolicy",  "--live '%s'" },           // Magisk
    { "/system/bin/magiskpolicy",       "--live '%s'" },
    { "/sbin/magiskpolicy",             "--live '%s'" },
    { "/system/bin/supolicy",           "--live '%s'" },           // SuperSU
};

bool exists(const char* p)
{
    struct stat st{};
    return ::stat(p, &st) == 0;
}

bool run(const std::string& cmd)
{
    int rc = ::system(cmd.c_str());
    return rc == 0;
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

    for (const auto& tool : kTools) {
        if (!exists(tool.path)) continue;

        KITTY_LOGI("sepolicy: using %s", tool.path);

        bool all = true;
        for (const auto& r : rules) {
            char args[768];
            snprintf(args, sizeof(args), tool.argfmt, r.statement.c_str());

            std::string cmd = std::string(tool.path) + " " + args + " >/dev/null 2>&1";
            KITTY_LOGI("sepolicy:   %s", r.statement.c_str());
            KITTY_LOGI("sepolicy:     (%s)", r.reason.c_str());

            if (!run(cmd)) {
                KITTY_LOGW("sepolicy:   tool reported failure for this rule");
                all = false;
            }
        }

        // Report the tool even on partial failure; the caller re-checks the AVC.
        if (toolUsed) *toolUsed = tool.path;
        return all;
    }

    KITTY_LOGE("sepolicy: no policy patch tool found "
               "(looked for ksud, magiskpolicy, supolicy).");
    return false;
}

}  // namespace recam::sepolicy
