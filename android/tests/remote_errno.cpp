// Native integration check: remote calls must not change the borrowed thread's errno.
#include "../src/injector/target.hpp"
#include <sys/mman.h>
#include <sys/wait.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <cerrno>
#include <cstdio>
#include <cassert>

__attribute__((noinline)) static uintptr_t poison_errno() {
    errno = ENOENT;
    return 123;
}

int main() {
    auto* state = static_cast<int*>(mmap(nullptr, 4096, PROT_READ | PROT_WRITE,
                                        MAP_SHARED | MAP_ANONYMOUS, -1, 0));
    assert(state != MAP_FAILED);
    int failures = 0;
    for (int i = 0; i < 10; ++i) {
        __atomic_store_n(state, 0, __ATOMIC_SEQ_CST);
        const pid_t child = fork();
        assert(child >= 0);
        if (!child) {
            errno = EINTR;
            __atomic_store_n(state, 1, __ATOMIC_SEQ_CST);
            while (__atomic_load_n(state, __ATOMIC_SEQ_CST) != 2)
                syscall(SYS_getpid);
            _exit(errno == EINTR ? 0 : 1);
        }
        while (!__atomic_load_n(state, __ATOMIC_SEQ_CST)) usleep(1000);
        KittyMemoryMgr kmgr;
        assert(recam::target::attach(kmgr, child, 36, 5000, nullptr));
        assert(kmgr.initialize(child, EK_MEM_OP_SYSCALL, true));
        const auto call = kmgr.trace.callFunction(reinterpret_cast<uintptr_t>(&poison_errno));
        assert(call.status == KT_RP_CALL_SUCCESS && call.result.val == 123);
        recam::target::release(kmgr, child);
        __atomic_store_n(state, 2, __ATOMIC_SEQ_CST);
        int status = 0;
        assert(waitpid(child, &status, 0) == child);
        if (!WIFEXITED(status) || WEXITSTATUS(status)) ++failures;
    }
    munmap(state, 4096);
    printf("remote errno: %d / 10 failed\n", failures);
    return failures ? 1 : 0;
}
