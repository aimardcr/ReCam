# ReCam - ndk-build application config

# x86_64 exists to prove the chain end-to-end on the emulator; arm64-v8a is the target.
APP_ABI      := arm64-v8a x86_64
APP_PLATFORM := android-30
APP_OPTIM    := release
APP_PIE      := true

# Static: the payload is dlopen'd into cameraserver, which has its own libc++.
APP_STL := c++_static

APP_CFLAGS   := -O2 -DNDEBUG -Wall -Wextra -fvisibility=hidden
APP_LDFLAGS  := -llog

APP_BUILD_SCRIPT := $(call my-dir)/Android.mk
