#include "../src/injector/staged_file.hpp"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <sys/mman.h>

int main() {
#ifdef __ANDROID__
    char androidDir[] = "/data/local/tmp/recam-stage-XXXXXX";
    const char* root = mkdtemp(androidDir);
#else
    char dir[] = "/tmp/recam-stage-XXXXXX";
    const char* root = mkdtemp(dir);
#endif
    assert(root);
    const std::string source = std::string(root) + "/input.so";
    auto write_source = [&](char value) {
        int fd = open(source.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0600);
        assert(fd >= 0);
        char data[4096]; memset(data, value, sizeof(data));
        assert(write(fd, data, sizeof(data)) == sizeof(data)); close(fd);
    };
    write_source('A');
    std::string first = recam::stage_payload_copy(source, root);
    assert(!first.empty());
    int fd = open(first.c_str(), O_RDONLY); assert(fd >= 0);
    struct stat before{}; assert(fstat(fd, &before) == 0);
    auto* mapping = static_cast<char*>(mmap(nullptr, 4096, PROT_READ, MAP_PRIVATE, fd, 0));
    assert(mapping != MAP_FAILED); close(fd);
    write_source('B');
    std::string second = recam::stage_payload_copy(source, root);
    assert(!second.empty() && first != second);
    struct stat after{}; assert(stat(second.c_str(), &after) == 0);
    assert(before.st_ino != after.st_ino && (after.st_mode & 0777) == 0644);
    for (int i = 0; i < 4096; ++i) assert(mapping[i] == 'A');
    {
        recam::StagedFileCleanup a{first}, b{second};
    }
    assert(access(first.c_str(), F_OK) < 0 && access(second.c_str(), F_OK) < 0);
    assert(mapping[0] == 'A'); // unlink does not invalidate retained mappings
    assert(recam::stage_payload_copy(source + ".missing", root).empty());
    munmap(mapping, 4096); unlink(source.c_str()); rmdir(root);
    puts("fresh staging inode, mapping preservation, and unlink checks passed");
}
