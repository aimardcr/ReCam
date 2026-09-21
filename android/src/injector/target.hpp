// Shared cameraserver handling for the injector tools.

#pragma once

#include <sys/types.h>
#include <stdint.h>
#include <string>

#include <KittyMemoryMgr.hpp>

#include "../common/control.hpp"

namespace recam::target {

// Name of the process both tools operate on.
extern const char* const kProcName;

int         sdk_level();
std::string proc_name(pid_t pid);
pid_t       find(const char* name);

// Found via the named anonymous mapping; a memfd dlopen leaves no stable path.
uintptr_t find_control_block(pid_t pid);

// Stop all threads, resume the main one, and attach; always resumed on failure.
bool attach(KittyMemoryMgr& kmgr, pid_t pid, int sdk, int timeoutMs, bool* seizedOut);

// Detach AND SIGCONT. Must be called on every path that attached.
void release(KittyMemoryMgr& kmgr, pid_t pid);

// Read the control block and validate it. False if absent or not ours.
bool read_control(KittyMemoryMgr& kmgr, uintptr_t ctrlAddr, recam::ControlBlock* out);

// Call recam_shutdown() remotely, then confirm from the control block.
bool shutdown_payload(KittyMemoryMgr& kmgr, uintptr_t ctrlAddr);

}  // namespace recam::target
