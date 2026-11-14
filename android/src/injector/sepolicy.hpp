// SELinux helpers for the ReCam injector.

#pragma once

#include <sys/types.h>
#include <cstdint>
#include <string>
#include <vector>

namespace recam::sepolicy {

// True if selinuxfs is mounted and enforcing state is readable.
bool available();

// True if SELinux is currently enforcing (false = permissive globally).
bool enforcing();

// Security context of a process, e.g. "u:r:cameraserver:s0". Empty on failure.
std::string process_context(pid_t pid);

// "u:r:cameraserver:s0" -> "cameraserver". Empty if the context is malformed.
std::string context_type(const std::string& context);

enum class Access { Unknown, Denied, Allowed };
// Query failure stays distinct from a policy denial.
Access permission(const std::string& scon, const std::string& tcon,
                  const char* cls, const char* perm);

// Answered by the kernel's own security_compute_av; permissive counts as granted.
bool has_permission(const std::string& scon, const std::string& tcon,
                    const char* cls, const char* perm);

// A policy rule in magiskpolicy/ksud statement form, plus why it is needed.
struct Rule {
    std::string statement;  // "allow cameraserver cameraserver_tmpfs file execute"
    std::string reason;     // human-readable justification for the log
};

// Relabel with chcon; false if the tool is missing or the label did not take.
bool relabel(const std::string& path, const char* context);

// Prepare only the file permissions needed by installed Qualcomm C2D libraries.
void prepare_c2d(pid_t targetPid);

// Apply to the live policy via ksud, magiskpolicy or supolicy.
bool apply_live(const std::vector<Rule>& rules, std::string* toolUsed);

}  // namespace recam::sepolicy
