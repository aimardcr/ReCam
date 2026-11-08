#include "../src/payload/inlinehook.hpp"
#include <cassert>
#include <cstdio>
#include <initializer_list>
extern "C" int recam_test_plain(int);
extern "C" int recam_test_bti(int);
extern "C" int recam_test_pac(int);
static uintptr_t original;
static int replacement(int x) { return reinterpret_cast<int(*)(int)>(original)(x) + 100; }
int main() {
    for (auto target : {recam_test_plain, recam_test_bti, recam_test_pac}) {
        recam::InlineHook hook;
        uintptr_t first = 0;
        for (unsigned i = 0; i < 1000; ++i) {
            assert(recam::hook_install(reinterpret_cast<uintptr_t>(target),
                   reinterpret_cast<void*>(replacement), &hook) == recam::HookResult::Ok);
            original = hook.trampoline;
            if (!first) first = original;
            assert(original == first && target(5) == 106);
            assert(recam::hook_remove(&hook) == recam::HookResult::Ok);
            assert(target(5) == 6);
            // A delayed old callback still has a valid original trampoline.
            assert(reinterpret_cast<int(*)(int)>(original)(5) == 6);
        }
        assert(recam::hook_install(reinterpret_cast<uintptr_t>(replacement),
               reinterpret_cast<void*>(replacement), &hook) == recam::HookResult::BadArgs);
    }
    puts("3000 reinstall cycles preserved trampoline addresses and original behavior");
}
