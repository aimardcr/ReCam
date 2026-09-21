// Executable memory for hook trampolines, without needing SELinux `execmem`.
#pragma once

#include <stddef.h>

namespace recam {

// Reserve `len` bytes of executable space. Returns nullptr when exhausted.
void* tramp_alloc(size_t len);

// Verifies by read-back; false means executable memory could not be written.
bool tramp_write(void* dst, const void* src, size_t len);

}  // namespace recam
