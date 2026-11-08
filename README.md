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

`recam inject` resolves `android::camera3::Camera3OutputStream::returnBufferCheckedLocked`
in the running `cameraserver`, attaches with ptrace, and loads `librecam.so`. The
payload installs an inline hook on that function. Each time the camera service returns
a filled buffer to a stream, the hook locks the gralloc buffer, overwrites the YUV
planes, and lets the original function continue.

Frame metadata is never touched. Timestamps, exposure, focus state and sensor
characteristics are whatever the real camera reported, so the substitution does not
show up in capture results.

Pixels arrive through a shared memory ring that `recam feed` fills from a separate
process, so decoding and scaling never run inside the camera service.

## Features

- **Works with any camera app.** The hook is in the camera service, below the app layer.
- **Preview, stills and video recording.** Including `IMPLEMENTATION_DEFINED` buffers,
  where the layout is read from the gralloc driver rather than guessed.
- **Several stream sizes at once.** The ring keeps four size-keyed lanes, so an app
  running preview and analysis at different resolutions gets correct frames on both.
- **Live desktop streaming.** H.264 in FLV over TCP through `adb forward`, with
  complete-picture framing and raw Annex-B compatibility. A built-in RTMP
  listener accepts OBS and anything else that can publish.
- **Any local capture device.** Including OBS Virtual Camera.
- **Reversible.** `recam stop` removes the hook and restores the real camera without
  restarting `cameraserver`.
- **One payload at a time.** Re-injecting stands the previous copy down first.
- **Self-test.** `recam test` runs 63 checks inside the camera service covering the
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

This runs `ndk-build`, bundles `recam` and `librecam.so` into the package, and writes
`dist/ReCam`. `adb` and `ffmpeg` are not bundled and remain PATH dependencies.

## Sources

The console offers five. `adb` handles the transport; `ffmpeg` does any encoding.

| Source | What happens |
|---|---|
| Test pattern | Scrolling colour bars with a moving marker |
| RTMP | ffmpeg listens; publish to the Server and Stream Key shown beside the radio button |
| Video file | Pushed to `/data/local/tmp/recam_source.mp4` and looped on the device |
| FFmpeg | A path, a URL, or raw input flags if the text starts with `-` |
| Camera | Any local capture device, including OBS Virtual Camera |

### Feeding OBS in

**Virtual camera.** In OBS press *Start Virtual Camera*, then pick `OBS Virtual Camera`
under **Camera** and press Start. Nothing else to configure.

**RTMP.** Select **RTMP** and press Start first, so ffmpeg is listening. Then in OBS go
to *Settings -> Stream -> Service: Custom* and copy the Server and Stream Key shown
beside the radio button. The key must match exactly; the listener rejects any other.
Set *Output -> Encoder -> B-frames* to **0**, because ReCam synthesises timestamps in
decode order and B-frames would reorder the output.

The virtual camera is less to get wrong. RTMP is worth the extra steps only if you want
OBS to publish over the network. Desktop ffmpeg re-encodes that input using
the same streaming settings as other desktop sources.

## Command line

The console is a front end for one executable, `recam`, with these subcommands.
Keep `librecam.so` beside it; the shared library is the payload loaded into
`cameraserver`. Run `recam --help` for usage. `recam feed [FILE]` optionally selects a
video file; the other subcommands take no arguments.

| Tool | Does |
|---|---|
| `recam inject` | Loads the payload and installs the hook |
| `recam feed` | Fills the frame ring from a file, a live sender, or a built-in test pattern |
| `recam stop` | Removes the hook and restores the real camera |
| `recam status` | Prints state as JSON; read-only and safe to poll |
| `recam test` | Runs the self-test inside `cameraserver` |
| `recam watch` | Re-injects when `cameraserver` respawns |

```bash
adb push android/libs/arm64-v8a/recam android/libs/arm64-v8a/librecam.so /data/local/tmp/
adb shell su -c 'chmod 755 /data/local/tmp/recam'
adb shell su -c '/data/local/tmp/recam inject'
adb shell su -c '/data/local/tmp/recam feed'     # Ctrl-C to stop
adb shell su -c '/data/local/tmp/recam stop'
```

`recam feed` plays `/data/local/tmp/recam_source.mp4` if it exists, accepts an H.264
stream on `127.0.0.1:27183`, and otherwise generates a test pattern. A live sender takes
priority over the file.

The desktop sends FLV/AVC with one complete H.264 picture per tag and immediate
packet flushing. The feeder recognizes FLV or raw Annex-B from the stream prefix;
existing raw senders still work. Explicit tag lengths let FLV pictures reach the
codec without waiting for the next picture. The device endpoint is plain TCP,
not an RTMP server. The supported FLV profile has one SPS/PPS pair, 1/2/4-byte NAL
lengths, no B-frame composition offset, and stable codec parameters per connection.
Malformed, oversized, truncated, or unsupported input closes that connection.
Reconnect to change codec parameters. At TCP EOF, queued output drains to codec
EOS, with a two-second limit, before fallback resumes.

Scaling preserves the existing centre crop and bilinear pixel values. A crop
already matching the output size uses row copies. Resampling caches horizontal
rows and uses NEON for the vertical interpolation on ARM.

Live input is serviced independently of the file playback tick. The feeder drains
ready codec output before and between nonblocking input submissions and publishes
the newest decoded frame available. It polls socket readiness with a 2 ms timeout
for checking codec output; this is not a guarantee of OS scheduling latency.
Older decoded frames may be superseded, while encoded reference pictures are kept.
Camera callbacks pin their source slot until copying finishes. The producer
never overwrites a pinned slot or the current publication; if no other slot is
available, it skips that decoded publication and retains the last complete frame.
`recam status` exposes this as `ring.producer_skipped`. Sequence numbers can skip
occupied slots and are identifiers, not a count of input frames. Lane resizing
and retirement cannot change geometry under a reader. An exclusive ring-fd lock
permits one producer and lets a replacement recover abandoned writes after a crash.
A short input gap retains the last published frame. After two seconds without a
new live frame, the feeder resumes paced file/pattern output; disconnecting also
restores that fallback. File playback retains its original frame interval.
If an encoded picture cannot be submitted or the compressed backlog exceeds the
limit, the connection is closed instead of silently skipping reference pictures.

### Lifecycle failures

`recam inject` exits nonzero if initialization fails or the payload does not report
an active hook. A missing hook target is an error. Failed installation attempts
cleanup; ownership is released only if cleanup succeeds.

`recam stop` exits nonzero if hook removal fails. The payload keeps its ownership
and shutdown entry point so removal can be retried, and another copy cannot take
over while that ownership remains. The CLI also checks that both the active flag
and hook count are cleared before reporting a successful stop. Source pointers and
the trampoline remain valid for callbacks already running when the hook is removed.
Substitution is then disabled, and the frame-ring mapping and fd are released when
the final in-flight callback returns. Stop never waits on a callback because the
injector may have interrupted that very thread. Payload images and trampolines
remain mapped as a reusable resident image. A later start with the same GNU ELF
build ID reuses that image and its immutable original trampoline after ring
retirement completes. The injector resumes the process between bounded retries
if callbacks are still retiring. It does not wait inside a borrowed camera thread.
This removes per-start image/trampoline allocation; it does not unload older
images. A changed build loads a fresh image, and mappings retained by older builds
still require process restart to reclaim.
The SELinux-compatible file-loading fallback stages a fresh inode/path for a new
load and unlinks it afterward. It never overwrites a still-mapped payload.

After closing codecs, sockets, and mappings, `recam feed` flushes diagnostics and
exits without running process-global destructors. This avoids an observed Android
buffer-pool teardown race: detached eviction threads can access mutexes destroyed
during normal process exit. Other subcommands keep normal exit behavior.

The desktop reports failed inject/stop commands as failed jobs. An injection error
stops the Start operation before it launches the feeder or desktop stream.

Remote commands also preserve the borrowed thread's `errno` before resuming it.
Without this, an interrupted Binder call can observe an error left by injection
and abort `cameraserver`, even when payload initialization succeeded.

### Regression checks

Close camera previews and stop the feeder before running `recam test` after
injection. It exercises the real hook, buffer, and frame-ring paths inside
`cameraserver` (63 checks on the verified device) and temporarily changes ring state.
`recam test` rejects a running feeder or any active camera client before attaching.
It also refuses to run if the bounded `dumpsys media.camera` query fails or its
client-list format is unrecognized. Keep cameras and feeders stopped for the full
test: this preflight is a snapshot, not a lock against newly starting clients.

Separate lifecycle tests simulate hook installation and removal failures in a
standalone Android process, including failed cleanup, retry behavior, and removal
during an in-flight source callback. They do
not inject faults into `cameraserver`. Build and run instructions are in
[android/tests/README.md](android/tests/README.md).

Desktop error-propagation tests require only Python's standard library:

```bash
python3 -m unittest discover -s desktop/tests -v
```

### Lifecycle soak

With no active ReCam session and an unlocked camera preview open, run:

```bash
python3 tools/stress_lifecycle.py --serial DEVICE_SERIAL --cycles 8 \
  --output .scratch/stability/run
```

Requires `adb` and `ffmpeg`. The runner deploys the current build, generates a
small fallback clip, and exercises disconnect, stop during streaming, pause/resume,
and reconnect with both file and pattern fallback. It checks actual live-frame
consumption, stable `cameraserver` PID, stopped state, and fatal logs as well as
exit status. The observed platform abort sometimes returned exit code zero.
Raw logs and a JSON report are saved, including partial results on failure.
For a time-based mixed-resolution run, use `--minutes 30 --alternate-sizes`.
`--minutes` overrides `--cycles` and finishes the current cycle after the duration.
Keep the preview active throughout; the runner records stopped-state camera-service
RSS and receive-to-copy frame age, including deliberate stalls and repeated frames.
It writes `progress.json` after each completed cycle. `--adb-root` supports a
userdebug emulator with root adbd; dynamically allocated host ports allow separate
devices to be tested concurrently.

## Latency profiling

Build and deploy the current binaries, stop any existing ReCam session, and leave
an unlocked device's camera preview open. This opt-in benchmark injects the payload,
starts a profiled feeder, and sends a synthetic H.264 stream using the desktop's
existing encoder settings:

```bash
python tools/profile_latency.py --serial DEVICE_SERIAL --seconds 25 \
  --size 1280x720 --output .scratch/latency/run-720p
```

Choose a new output directory for each run. The tool stops its feeder and removes
the hook afterward. It saves raw device logs, host byte timestamps, the FFmpeg
command, clock samples, and a JSON report with p50/p95/p99 timings. Run with
`--size 1920x1080` to compare a second source size; output sizes come from the camera
client and are recorded in the report.

The measured host endpoint is encoded bytes arriving at a local TCP proxy; the
device endpoint is completion of the copy into camera YUV planes. Capture,
encoding, buffer unlock, app delivery, and display are excluded. Device-only stage
timings use `CLOCK_MONOTONIC`. Host/device estimates include a clock uncertainty
bound from status round trips before and after the run; they do not isolate pure
USB/ADB transport latency. The proxy and per-frame logs add measurement overhead.
Startup (first three seconds) and the nominal final second are excluded. Queue-to-
dequeue time includes codec buffering and feeder polling, not just decoder work.

Profiling is off by default. For manual collection, start the feeder with
`RECAM_PROFILE=1 /data/local/tmp/recam feed` and collect `adb logcat -s ReCamLatency`.
The feeder enables consumer logging through shared-memory ring version 4; rebuild
both binaries and re-inject when upgrading from version 2 or 3. Version 4 adds
reader ownership and per-slot publication metadata. The profiler defaults
to FLV; pass `--transport annexb` for a raw-stream comparison. Both use the same
encoder settings and measurement endpoints.

Measured baseline and limitations: [baseline report](benchmarks/latency-2026-11-08.md).
After the live scheduling changes: [comparison report](benchmarks/scheduling-2026-11-08.md).
Follow-up teardown diagnosis and validation:
[stability report](benchmarks/stability-2026-11-08.md).
Framing, scaling, and extended-soak results:
[transport and scaling report](benchmarks/transport-2026-11-08.md).

### Source-to-compositor sampling

With a live preview open and ReCam stopped:

```bash
python3 -m venv .scratch/profile-env
.scratch/profile-env/bin/pip install pillow qrcode opencv-python-headless
.scratch/profile-env/bin/python tools/profile_compositor.py \
  --serial DEVICE_SERIAL --size 1280x720 --samples 30 \
  --output .scratch/compositor/run-720p
```

This sends synthetic frames with QR identifiers and records their host generation
times. Each ADB screenshot is matched to its frame; request start and first PNG-header arrival bound the
age of that frame at compositor sampling without synchronizing device clocks.
The measurement includes QR generation, encoding, forward transport, camera delivery,
and app composition. It excludes physical source capture and panel response, and
snapshot frame age is distinct from the delay to its first presentation. Header
arrival is an upper bound, not the exact sampling instant; full screenshot round
trip is recorded separately. This is not an optical end-to-end measurement.
Raw screenshots, per-source-frame generation times, and JSON evidence stay in
the selected output directory. Unmatched images fail the measurement instead of
being reported as zero latency. Use
`--adb-root` for a rooted-adbd emulator. The source and hook are stopped afterward.

Extended stability and Android 12 compatibility results:
[extended validation report](benchmarks/extended-2026-11-08.md).
Follow-up slot ownership, resident reuse, and crash-recovery validation:
[reliability report](benchmarks/reliability-2026-11-08.md).

Analysis regression checks:

```bash
python3 -m unittest discover -s tools/tests -v
c++ -std=c++17 android/tests/latency.cpp -o /tmp/recam-timeline-test
/tmp/recam-timeline-test
```

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
