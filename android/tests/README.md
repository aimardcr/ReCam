# Lifecycle regression checks

`lifecycle.cpp` compiles the production payload lifecycle into a standalone
Android executable with controllable hook-operation failures. It checks failed
initialization, cleanup, ownership preservation, takeover rejection, and retries.
It does not inject into or modify `cameraserver`.

With NDK r27 installed, run from the repository root on a macOS host:

```sh
"$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/darwin-x86_64/bin/aarch64-linux-android30-clang++" \
  -std=c++17 -static-libstdc++ -Wall -Wextra -Werror android/tests/lifecycle.cpp \
  -llog -ldl -o /tmp/recam-lifecycle-test
adb push /tmp/recam-lifecycle-test /data/local/tmp/
adb shell /data/local/tmp/recam-lifecycle-test
python3 -m unittest discover -s desktop/tests -v
```

For a Linux host, use the `linux-x86_64` NDK prebuilt directory. The native test
above requires an ARM64 Android device. The desktop tests need only Python's
standard library and check that payload errors reach the UI's existing exception
handler while successful calls retain their result contract.

The existing `recam test` command separately validates the actual hooks and buffer
operations inside `cameraserver` after `recam inject`. Close camera previews and
stop the feeder first: this test changes ring state. The CLI now rejects active
camera clients, active feeders, and camera-service state it cannot verify before
attaching. Keep other clients stopped until it finishes.

## Decoder scheduling

This standalone test compiles the production decoder with deterministic wrapped
MediaCodec buffer APIs. It needs no camera and tests nonblocking scheduling,
newest-output selection, reference-picture error handling, fragmented FLV tag
submission without lookahead, input backpressure, and codec EOS draining:

```sh
"$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/darwin-x86_64/bin/aarch64-linux-android30-clang++" \
  -std=c++20 -static-libstdc++ -Wall -Wextra -Werror -DkNO_KEYSTONE -DkUSE_LOGCAT \
  -Iandroid/third_party/AndKittyInjector/KittyMemoryEx/KittyMemoryEx \
  android/tests/decoder_scheduling.cpp android/src/feed/h264.cpp \
  -lmediandk -llog -ldl \
  -Wl,--wrap=AMediaCodec_dequeueInputBuffer,--wrap=AMediaCodec_getInputBuffer \
  -Wl,--wrap=AMediaCodec_queueInputBuffer,--wrap=AMediaCodec_dequeueOutputBuffer \
  -Wl,--wrap=AMediaCodec_getOutputBuffer,--wrap=AMediaCodec_releaseOutputBuffer \
  -o /tmp/recam-decoder-test
adb push /tmp/recam-decoder-test /data/local/tmp/
adb shell /data/local/tmp/recam-decoder-test
```

## In-flight hook shutdown

This test removes the hook from inside a source's `fill()` call and then lets the
production callback finish. It checks that shutdown retains the static source
pointers needed by a callback already in progress, without injecting a fault into
`cameraserver`. It also checks failed-removal ownership, late callbacks, and
exactly-once deferred reclamation with 16 concurrent callback references.

```sh
"$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/darwin-x86_64/bin/aarch64-linux-android30-clang++" \
  -std=c++17 -static-libstdc++ -Wall -Wextra -Werror android/tests/hook_shutdown.cpp \
  -llog -o /tmp/recam-hook-shutdown-test
adb push /tmp/recam-hook-shutdown-test /data/local/tmp/
adb shell /data/local/tmp/recam-hook-shutdown-test
```

## CLI exit and self-test preflight

These run on the host with a POSIX C++17 compiler (or on Android using the NDK):

```sh
c++ -std=c++17 -Wall -Wextra -Werror android/tests/cli_exit.cpp -o /tmp/recam-cli-exit-test
/tmp/recam-cli-exit-test
c++ -std=c++17 -Wall -Wextra -Werror android/tests/test_preflight.cpp -o /tmp/recam-preflight-test
/tmp/recam-preflight-test
```

The CLI test forks child processes through the production command dispatcher.
It verifies feed cleanup returns before exit, buffered output is flushed, feed
skips global destructors, exit codes are preserved, and other commands still run
normal exit handlers. The preflight parser distinguishes current clients from
historical connections and rejects missing, ambiguous, or truncated state.

For actual device lifecycle validation, see `tools/stress_lifecycle.py` and the
README's lifecycle-soak instructions. `python3 -m unittest discover -s tools/tests
-v` includes crash-log detection: a background-thread fatal must fail validation
even when the feeder's exit status is zero.

## Framing and scaling

Host checks (add `-fsanitize=address,undefined -g` for sanitizer coverage):

```sh
c++ -std=c++17 android/tests/flv.cpp android/src/feed/h264.cpp -o /tmp/recam-flv-test
/tmp/recam-flv-test
c++ -std=c++17 android/tests/scale.cpp android/tests/scale_reference.cpp \
  android/src/feed/scale.cpp -o /tmp/recam-scale-test
/tmp/recam-scale-test
python3 -m unittest discover -s desktop/tests -v
```

The framing test covers bounds, truncated configuration records, invalid tag
trailers, NAL lengths, picture boundaries, and deterministic randomized inputs.
For an actual muxer fixture, generate one second of 320×240 30 fps H.264 in FLV
with ffmpeg (`libx264`, `-preset ultrafast -tune zerolatency -bf 0`), then pass that
filename to `/tmp/recam-flv-test`. It verifies all 30 pictures and every partial
tag length. The scalar reference is the frozen pre-optimization algorithm.
Differential tests compare every byte and destination canaries over 160 layouts,
including crop-only, up/downscale, padding, and pixel strides 1–4.

Compile `android/tests/scale_benchmark.cpp` with `android/src/feed/scale.cpp` using
NDK `-O2 -std=c++17` and run it on the device for a standalone timing/checksum
measurement. Its timing excludes the real camera pipeline.

The ring-resource test uses actual memfds and mappings, checks 100 create/destroy
cycles, and verifies repeated cleanup cannot close an unrelated reused fd:

```sh
"$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/darwin-x86_64/bin/aarch64-linux-android30-clang++" \
  -std=c++17 -static-libstdc++ -Wall -Wextra -Werror android/tests/shm_cleanup.cpp \
  android/src/payload/shmsource.cpp -llog -o /tmp/recam-shm-cleanup-test
adb push /tmp/recam-shm-cleanup-test /data/local/tmp/
adb shell /data/local/tmp/recam-shm-cleanup-test
```

## Borrowed-thread errno

`remote_errno.cpp` forks ten disposable child processes, sets each child's errno
to `EINTR`, borrows its thread through the production attach/release path, and
executes a function that changes errno to `ENOENT`. Each child must resume with
its original errno. This needs root for ptrace; it never attaches to cameraserver.
After the normal ARM64 `ndk-build`, compile against its KittyMemoryEx objects:

```sh
"$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/darwin-x86_64/bin/aarch64-linux-android30-clang++" \
  -std=c++20 -static-libstdc++ -DkNO_KEYSTONE -DkUSE_LOGCAT \
  -Iandroid/third_party/AndKittyInjector/KittyMemoryEx/KittyMemoryEx \
  -Iandroid/third_party/AndKittyInjector/AndKittyInjector/src \
  android/tests/remote_errno.cpp android/src/injector/target.cpp \
  android/obj/local/arm64-v8a/objs/recam/third_party/AndKittyInjector/KittyMemoryEx/KittyMemoryEx/*.o \
  -llog -ldl -o /tmp/recam-remote-errno-test
adb push /tmp/recam-remote-errno-test /data/local/tmp/
adb shell su -c /data/local/tmp/recam-remote-errno-test
```

## File-loading fallback

`staged_file.cpp` checks fresh inode/path allocation, preservation of an older
mapping while a new payload is staged, and unlink without invalidating mappings.
It runs on the host or on Android using the NDK:

```sh
c++ -std=c++17 -Wall -Wextra -Werror android/tests/staged_file.cpp -o /tmp/recam-stage-test
/tmp/recam-stage-test
```

## Real decoder integration

`decoder_stream.cpp` uses the production parser and actual MediaCodec byte buffers
without cameraserver or gralloc. Generate matching one-second 320×240 30 fps
`testsrc2` fixtures using libx264 ultrafast/zerolatency and `-bf 0`, in FLV and raw
H.264. The test verifies dimensions, nonempty decoded pixels, and stream completion.
Check logcat for codec errors or the two-second EOS timeout as well as exit status.

```sh
"$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/darwin-x86_64/bin/aarch64-linux-android30-clang++" \
  -O2 -std=c++20 -static-libstdc++ -DkNO_KEYSTONE -DkUSE_LOGCAT \
  -Iandroid/third_party/AndKittyInjector/KittyMemoryEx/KittyMemoryEx \
  android/tests/decoder_stream.cpp android/src/feed/decoder.cpp android/src/feed/h264.cpp \
  -lmediandk -llog -o /tmp/recam-stream-test
adb push /tmp/recam-stream-test fixture.flv fixture.h264 /data/local/tmp/
adb shell /data/local/tmp/recam-stream-test /data/local/tmp/fixture.flv /data/local/tmp/fixture.h264
```

Native test commands link the C++ runtime statically so a clean emulator does not
need a separately deployed `libc++_shared.so`. API 31 emulator component tests passed;
its full gralloc self-test crashed the host graphics emulator. See the
[compatibility report](../../benchmarks/extended-2026-11-08.md) for the exact scope.

## Reader ownership and resident reuse

`ring_access.cpp` pins a frame across 10,000 newer publications, saturates all
slots, aborts a write, and checks concurrent copies. It runs on the host and Android:

```sh
c++ -std=c++17 -O1 -g -pthread -fsanitize=address,undefined \
  android/tests/ring_access.cpp -o /tmp/recam-ring-test
/tmp/recam-ring-test
```

Run a separate build with `-fsanitize=thread` for race detection. `ring_restart.cpp`
uses a real shared memfd and a killed child producer to verify exclusive flock
ownership, abandoned-write recovery, and preservation of reader pins. On Android:

```sh
"$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/darwin-x86_64/bin/aarch64-linux-android30-clang++" \
  -O1 -std=c++17 -static-libstdc++ -Wall -Wextra -Werror \
  android/tests/ring_restart.cpp -o /tmp/recam-ring-restart-test
adb push /tmp/recam-ring-restart-test /data/local/tmp/
adb shell /data/local/tmp/recam-ring-restart-test
```

`hook_shutdown.cpp` also checks 100 retirement/restart transitions, refusal while
a callback is active, and late disabled callbacks during restart preparation.
`hook_reuse.cpp` reinstalls each of three actual ARM64 hooks 1,000 times, verifies
that trampoline addresses never change, and calls the original after removal.
The full self-test also caches its three hook records across invocations:

```sh
"$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/darwin-x86_64/bin/aarch64-linux-android30-clang++" \
  -O1 -std=c++17 -static-libstdc++ -Wall -Wextra -Werror \
  android/tests/hook_reuse.cpp android/src/payload/inlinehook_arm64.cpp \
  android/src/payload/tramp_alloc.cpp android/src/payload/tramp_arena.S \
  android/src/payload/selftest_targets.S -llog -o /tmp/recam-hook-reuse-test
adb push /tmp/recam-hook-reuse-test /data/local/tmp/
adb shell /data/local/tmp/recam-hook-reuse-test
```

`payload_identity.cpp` checks truncated/malformed GNU notes and exact build-ID
matching. Compile it with the same NDK C++17/static-runtime flags, then run it
with `/data/local/tmp/librecam.so` as its argument. It also runs on ELF hosts;
macOS lacks the required system `elf.h`.

The [reliability report](../../benchmarks/reliability-2026-11-08.md) records
same-build mapping/RSS checks, live switching, feeder restarts, and screenshot
sampling. Old images are not unloaded; same-build starts reuse one resident image.
