// Real process death while holding producer locks; readers remain pinned.
#include "../src/common/ring_access.hpp"
#include <cassert>
#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
int main() {
    const int fd = syscall(SYS_memfd_create, "recam_restart_test", 0);
    assert(fd >= 0 && ftruncate(fd, 4096) == 0);
    auto* h = static_cast<recam::ShmHeader*>(mmap(nullptr, 4096,
        PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));
    assert(h != MAP_FAILED);
    {
        recam::RingPublication initial(h->lane[0]);
        initial.publish(123);
    }
    {
        recam::RingRead access(&h->lane[0].access);
        recam::RingFrame reader(h->lane[0]);
        assert(reader.seq);
        h->lane[2].access = recam::kWriter; // a camera-owned claim must survive
        int ready[2]; assert(pipe(ready) == 0);
        const pid_t child = fork(); assert(child >= 0);
        if (child == 0) {
            close(ready[0]);
            char path[64]; snprintf(path, sizeof(path), "/proc/self/fd/%d", fd);
            const int writerFd = open(path, O_RDWR);
            assert(writerFd >= 0 && flock(writerFd, LOCK_EX | LOCK_NB) == 0);
            recam::RingPublication partial(h->lane[0]);
            assert(partial.seq);
            h->lane[1].access = recam::kWriter + 1;
            h->lane[1].width = 640; h->lane[1].published = 99;
            assert(write(ready[1], "x", 1) == 1);
            for (;;) pause(); // parent kills us; no C++ destructors run
        }
        close(ready[1]);
        char byte; assert(read(ready[0], &byte, 1) == 1); close(ready[0]);
        assert(flock(fd, LOCK_EX | LOCK_NB) == -1 && errno == EWOULDBLOCK);
        assert(kill(child, SIGKILL) == 0);
        int status; assert(waitpid(child, &status, 0) == child && WIFSIGNALED(status));
        assert(flock(fd, LOCK_EX | LOCK_NB) == 0);
        recam::recover_dead_producer(*h);
        assert(h->lane[0].access == 1 && h->lane[0].slots[reader.slot] == 1);
        assert(h->lane[0].slotSeq[reader.slot] == reader.seq);
        assert(h->lane[1].access == 0 && h->lane[1].width == 0 && h->lane[1].published == 0);
        assert(h->lane[2].access == recam::kWriter);
        recam::RingPublication next(h->lane[0]);
        assert(next.seq); next.publish(456);
        assert(h->lane[0].slotNs[reader.slot] == 123);
    }
    munmap(h, 4096); close(fd);
    puts("SIGKILL recovery, producer exclusion, and surviving-reader checks passed");
}
