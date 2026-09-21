// ReCam control block - the rendezvous point between payload copies.

#pragma once

#include <stdint.h>

namespace recam {

// "RECAMCTL" little-endian.
static const uint64_t kControlMagic = 0x4C5443414D414345ULL;

static const uint32_t kControlVersion = 2;   // v2 added the shm frame ring fields

// Name passed to PR_SET_VMA_ANON_NAME; appears in maps as "[anon:recam]".
#define RECAM_MARKER_NAME   "recam"
#define RECAM_MARKER_MAPS   "[anon:recam]"

struct ControlBlock {
    uint64_t magic;        // kControlMagic
    uint32_t version;      // kControlVersion
    uint32_t generation;   // incremented each time a new copy takes over

    // 1 while a payload owns the process; cleared by recam_shutdown.
    volatile uint32_t active;
    uint32_t hooks_installed;  // how many hooks the active copy currently has in place

    // Address of the ACTIVE copy's recam_shutdown(). The injector calls this remotely.
    uint64_t shutdown_fn;

    // Load bias of the active copy, for logging and sanity checks.
    uint64_t owner_base;

    // Owning pid; the mapping dies with the process, so this cannot go stale.
    uint32_t owner_pid;

    // The shared frame ring (src/common/shmframe.hpp), if the payload created one.
    uint32_t shm_fd;        // 0 = no ring
    uint64_t shm_size;      // bytes, for mmap
};

static_assert(sizeof(ControlBlock) == 56, "ControlBlock layout changed");

}  // namespace recam
