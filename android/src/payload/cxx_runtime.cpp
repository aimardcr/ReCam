// Minimal C++ runtime stubs for the payload.

#include <android/log.h>
#include <stdlib.h>

extern "C" void __cxa_pure_virtual()
{
    __android_log_print(ANDROID_LOG_FATAL, "ReCam",
                        "pure virtual call - this is a bug; aborting rather than "
                        "running undefined code inside cameraserver");
    abort();
}

// Never called - nothing in the payload is heap-allocated through a base pointer.
void operator delete(void*) noexcept {}
void operator delete(void*, unsigned long) noexcept {}
