// Host-only shim for payload component tests. NDK builds use the real header.
#pragma once
#define ANDROID_LOG_INFO 4
#define ANDROID_LOG_WARN 5
#define ANDROID_LOG_ERROR 6
inline int __android_log_print(int, const char*, const char*, ...)
    __attribute__((format(printf, 3, 4)));
inline int __android_log_print(int, const char*, const char*, ...) { return 0; }
