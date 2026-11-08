# Lifecycle regression checks

`lifecycle.cpp` compiles the production payload lifecycle into a standalone
Android executable with controllable hook-operation failures. It checks failed
initialization, cleanup, ownership preservation, takeover rejection, and retries.
It does not inject into or modify `cameraserver`.

With NDK r27 installed, run from the repository root on a macOS host:

```sh
"$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/darwin-x86_64/bin/aarch64-linux-android30-clang++" \
  -std=c++17 -Wall -Wextra -Werror android/tests/lifecycle.cpp \
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
operations inside `cameraserver` after `recam inject`.
