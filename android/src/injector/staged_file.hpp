#pragma once

#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace recam {

// Each dlopen must see a fresh inode/path. Truncating a previously loaded file
// can corrupt still-mapped pages, and reusing its path can return a retired image.
inline std::string stage_payload_copy(const std::string& source, const std::string& directory)
{
    if (mkdir(directory.c_str(), 0755) < 0 && errno != EEXIST) return {};
    const int input = open(source.c_str(), O_RDONLY | O_CLOEXEC);
    if (input < 0) return {};
    std::string pattern = directory + "/librecam-XXXXXX";
    std::vector<char> name(pattern.begin(), pattern.end());
    name.push_back(0);
    const int output = mkstemp(name.data());
    if (output < 0) { close(input); return {}; }
    bool ok = true;
    char bytes[65536];
    for (;;) {
        const ssize_t count = read(input, bytes, sizeof(bytes));
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) { ok = false; break; }
        if (!count) break;
        ssize_t offset = 0;
        while (offset < count) {
            const ssize_t written = write(output, bytes + offset, count - offset);
            if (written < 0 && errno == EINTR) continue;
            if (written <= 0) { ok = false; break; }
            offset += written;
        }
        if (!ok) break;
    }
    if (ok && fchmod(output, 0644)) ok = false;
    if (close(output)) ok = false;
    close(input);
    if (!ok) { unlink(name.data()); return {}; }
    return name.data();
}

// Unlink after the remote loader has consumed the file, including failure paths.
// Existing mappings keep the inode alive; it is never overwritten or dlclosed.
struct StagedFileCleanup {
    const std::string& path;
    ~StagedFileCleanup() { if (!path.empty()) unlink(path.c_str()); }
};

} // namespace recam
