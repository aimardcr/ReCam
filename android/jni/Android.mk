# ReCam - ndk-build module definitions

LOCAL_PATH := $(call my-dir)/..

KITTY_ROOT := third_party/AndKittyInjector
KITTY_MEM  := $(KITTY_ROOT)/KittyMemoryEx/KittyMemoryEx
KITTY_INJ  := $(KITTY_ROOT)/AndKittyInjector/src

# XZ Embedded, for the LZMA-compressed .symtab inside .gnu_debugdata.
XZ_ROOT := third_party/xz-embedded

# $(wildcard) needs a real path, so glob absolutely and strip the prefix back off.
KITTY_MEM_SRC := $(subst $(LOCAL_PATH)/,,$(wildcard $(LOCAL_PATH)/$(KITTY_MEM)/*.cpp))

KITTY_SRC := \
    $(KITTY_INJ)/Injector/KittyInjector.cpp \
    $(KITTY_MEM_SRC)

ifeq ($(TARGET_ARCH_ABI),arm64-v8a)
  RECAM_HOOK_SRC     := src/payload/inlinehook_arm64.cpp
else ifeq ($(TARGET_ARCH_ABI),x86_64)
  RECAM_HOOK_SRC     := src/payload/inlinehook_x86_64.cpp
else
  $(error ReCam: no inline hook implementation for $(TARGET_ARCH_ABI))
endif


# ---------------------------------------------------------------- payload

include $(CLEAR_VARS)

LOCAL_MODULE := recam_payload
LOCAL_MODULE_FILENAME := librecam

LOCAL_SRC_FILES := \
    src/payload/payload.cpp \
    src/payload/framehook.cpp \
    src/payload/bufferlock.cpp \
    src/payload/qcom_ubwc.cpp \
    src/payload/capture_layout.cpp \
    src/payload/metadata_import.cpp \
    src/payload/colorbars.cpp \
    src/payload/shmsource.cpp \
    src/payload/cxx_runtime.cpp \
    src/payload/tramp_alloc.cpp \
    src/payload/tramp_arena.S \
    $(RECAM_HOOK_SRC)

LOCAL_CPPFLAGS += -std=c++17 -fno-exceptions -fno-rtti -Wall -Wextra -Werror
LOCAL_CPPFLAGS += -fvisibility=hidden -ffunction-sections -fdata-sections
LOCAL_LDLIBS   += -llog
LOCAL_LDFLAGS  += -Wl,--gc-sections -Wl,--exclude-libs,ALL

include $(BUILD_SHARED_LIBRARY)


# ---------------------------------------------------------------- multicall executable

include $(CLEAR_VARS)

LOCAL_MODULE := recam

LOCAL_SRC_FILES := \
    src/cli/main.cpp \
    src/injector/main.cpp \
    src/injector/stop.cpp \
    src/injector/status.cpp \
    src/injector/watch.cpp \
    src/injector/sepolicy.cpp \
    src/injector/elfsym.cpp \
    src/injector/target.cpp \
    src/feed/main.cpp \
    src/feed/decoder.cpp \
    src/feed/scale.cpp \
    src/feed/h264.cpp \
    $(XZ_ROOT)/linux/lib/xz/xz_crc32.c \
    $(XZ_ROOT)/linux/lib/xz/xz_dec_lzma2.c \
    $(XZ_ROOT)/linux/lib/xz/xz_dec_stream.c \
    $(KITTY_SRC)

LOCAL_C_INCLUDES += \
    $(LOCAL_PATH)/$(KITTY_MEM) \
    $(LOCAL_PATH)/$(KITTY_INJ) \
    $(LOCAL_PATH)/src/injector \
    $(LOCAL_PATH)/$(XZ_ROOT)/userspace \
    $(LOCAL_PATH)/$(XZ_ROOT)/linux/include/linux

# XZ_SINGLE-only build: one-shot decode of an in-memory blob, no BCJ filters.
LOCAL_CFLAGS += -DXZ_DEC_ANY_CHECK -Wno-unused-parameter

# kNO_KEYSTONE: AndKittyInjector builds without the keystone assembler by default.
LOCAL_CPPFLAGS += -std=c++20 -fexceptions -Wall -Wextra -DkNO_KEYSTONE -DkUSE_LOGCAT
LOCAL_LDLIBS   += -llog -lmediandk -landroid

include $(BUILD_EXECUTABLE)
