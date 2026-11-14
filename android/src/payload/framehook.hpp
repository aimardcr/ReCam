// The hook on Camera3OutputStream::returnBufferCheckedLocked.

#pragma once

#include <stdint.h>

namespace recam {

// `target` is the runtime address resolved by the injector, never by the payload.
bool framehook_prepare(uintptr_t target);
bool framehook_install(uintptr_t target);

bool framehook_remove();
uint32_t framehook_count();

// Which pixel formats the hook will substitute.
bool framehook_format_supported(int32_t format);

}  // namespace recam
