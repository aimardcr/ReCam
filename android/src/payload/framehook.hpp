// The hook on Camera3OutputStream::returnBufferCheckedLocked.

#pragma once

#include <stdint.h>

namespace recam {

// `target` is the runtime address resolved by the injector, never by the payload.
bool framehook_install(uintptr_t target);

bool framehook_remove();

// Which pixel formats the hook will substitute. Exposed for the self-test.
bool framehook_format_supported(int32_t format);

// Returns the failure count; 0 means everything passed.
int selftest_run();

}  // namespace recam
