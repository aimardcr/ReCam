# ReCam

Android virtual camera for authorized security testing. ReCam injects a payload into
`cameraserver` and replaces the pixels in camera buffers before they reach the app, so
every camera client on the device sees whatever you feed it: a video file, a live
desktop stream, an RTMP publisher, or a webcam.

Nothing is modified in the target app. No Xposed, no repackaging, no per-app hooks. The
substitution happens once, in the camera service, and applies to everything.

Root is required.

## Legitimate use

This is a tool for testing systems you own or have written authorization to test.
Camera input is a trust boundary for KYC, liveness, document capture, and attendance
systems, and the only way to know whether one can be fooled is to try it against your
own deployment. Using it against services you do not own or have permission to test is
likely illegal where you live.

## How it works

`recam_inject` resolves `android::camera3::Camera3OutputStream::returnBufferCheckedLocked`
in the running `cameraserver`, attaches with ptrace, and loads `librecam.so`. The
payload installs an inline hook on that function. Each time the camera service returns
a filled buffer to a stream, the hook locks the gralloc buffer, overwrites the YUV
planes, and lets the original function continue.

Frame metadata is never touched. Timestamps, exposure, focus state and sensor
characteristics are whatever the real camera reported, so the substitution does not
show up in capture results.

Pixels arrive through a shared memory ring that `recam_feed` fills from a separate
process, so decoding and scaling never run inside the camera service.

## Features

- **Works with any camera app.** The hook is in the camera service, below the app layer.
- **Preview, stills and video recording.** Including `IMPLEMENTATION_DEFINED` buffers,
  where the layout is read from the gralloc driver rather than guessed.
- **Several stream sizes at once.** The ring keeps four size-keyed lanes, so an app
  running preview and analysis at different resolutions gets correct frames on both.
- **Live desktop streaming.** H.264 over TCP through `adb forward`. A built-in RTMP
  listener accepts OBS and anything else that can publish.
- **Any local capture device.** Including OBS Virtual Camera.
- **Reversible.** `recam_stop` removes the hook and restores the real camera without
  restarting `cameraserver`.
- **One payload at a time.** Re-injecting stands the previous copy down first.
- **Self-test.** `recam_test` runs 63 checks inside the camera service covering the
  hook, buffer layouts, fence ownership and the frame ring.

## Requirements

**Device**

- Rooted, with a working `su`
- arm64-v8a, Android 11 (API 30) or newer
- USB debugging

**Host**

- `adb` on PATH
- `ffmpeg` on PATH, for the live, RTMP and camera sources
- Python 3.9+ and PySide6, for the desktop console

## Verification status

The full chain is verified on one physical device: Android 16, arm64-v8a, MediaTek
(PowerVR gralloc). Symbol resolution is verified across Android 11 through 16, and the
dynamic-linkage path is verified on an Android 14 x86_64 emulator.

Gralloc behaviour on Qualcomm and Exynos is untested. Where the device cannot describe
a buffer, the hook passes the real frame through rather than writing a guess, so an
unsupported device shows the real camera instead of corrupted output.

## Building

**Build requirements**

- Android NDK r27 or newer (tested with r27.3.13750724)
- Python 3.9+
- PyInstaller, only for a frozen release

Device binaries:

```bash
cd android
ndk-build NDK_PROJECT_PATH=. NDK_APPLICATION_MK=jni/Application.mk -j8
```

Output lands in `android/libs/arm64-v8a`. The desktop console finds it there
automatically, so this is enough for development.

Desktop console:

```bash
cd desktop
pip install -r requirements.txt
python main.py
```

Pick a device, pick a source, press Start. The console pushes the device binaries when
their content hash differs from what is already on the device, so there is no separate
deploy step.

A frozen release with the device binaries bundled:

```bash
export ANDROID_NDK_HOME=/path/to/ndk
python tools/package.py
```

This runs `ndk-build`, folds the six device binaries into the package, and writes
`dist/ReCam`. `adb` and `ffmpeg` are not bundled and remain PATH dependencies.

## Command line

The console is a front end for these. They take no arguments.

| Tool | Does |
|---|---|
| `recam_inject` | Loads the payload and installs the hook |
| `recam_feed` | Fills the frame ring from a file, a live sender, or a built-in test pattern |
| `recam_stop` | Removes the hook and restores the real camera |
| `recam_status` | Prints state as JSON; read-only and safe to poll |
| `recam_test` | Runs the self-test inside `cameraserver` |
| `recam_watch` | Re-injects when `cameraserver` respawns |

```bash
adb push android/libs/arm64-v8a/* /data/local/tmp/
adb shell su -c 'chmod 755 /data/local/tmp/recam_*'
adb shell su -c '/data/local/tmp/recam_inject'
adb shell su -c '/data/local/tmp/recam_feed'     # Ctrl-C to stop
adb shell su -c '/data/local/tmp/recam_stop'
```

`recam_feed` plays `/data/local/tmp/recam_source.mp4` if it exists, accepts an H.264
stream on `127.0.0.1:27183`, and otherwise generates a test pattern. A live sender takes
priority over the file.

## Layout

```
android/     device side, C++ and ndk-build
  jni/       Android.mk, Application.mk
  src/       common, payload, injector, feed
  sepolicy/  the two SELinux rules, both from observed denials
  patches/   local change to the vendored injector
desktop/     host side, Python and PySide6
tools/       build and release scripts
```

## Third-party code

- [AndKittyInjector](https://github.com/MJx0/AndKittyInjector) by MJ, MIT. Vendored at
  `6d55e9f` with one local patch, see `android/patches`.
- [XZ Embedded](https://github.com/tukaani-project/xz-embedded), public domain, used to
  read `.gnu_debugdata` when the target symbol is not exported.

## Licence

MIT. See [LICENSE](LICENSE).
