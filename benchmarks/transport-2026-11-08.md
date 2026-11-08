# Framing, scaling, and lifecycle validation — 2026-11-08

On the POCO 2409FPCC4G (Android 16/API 36, ARM64), FLV framing removes roughly
one frame of Annex-B boundary lookahead. The optimized scaler reduces additional
CPU work while preserving the previous pixel output. TCP over ADB is unchanged.

## Latency comparison

Each comparison ran for 25 seconds with synthetic 30 fps input, libx264 ultrafast,
zerolatency, no B frames, and a 1440×1080 camera output lane. Startup (three
seconds) and the nominal final second are excluded. Times below are milliseconds.

| Input / implementation | Receive → queue p50 | Scale p50 | Receive → camera copy p50 | p95 | p99 |
|---|---:|---:|---:|---:|---:|
| 720p Annex-B, original scaler | 35.718 | 12.419 | 92.230 | 121.008 | 144.938 |
| 720p FLV, original scaler | 1.258 | 12.811 | 60.363 | 87.994 | 110.402 |
| 720p FLV, optimized scaler | 1.307 | 7.490 | 57.119 | 85.826 | 93.937 |
| 1080p Annex-B, original scaler | 35.473 | 12.593 | 98.712 | 127.693 | 145.561 |
| 1080p FLV, original scaler | 1.429 | 12.934 | 68.059 | 97.606 | 137.835 |
| 1080p FLV, optimized scaler | 2.176 | 1.181 | 56.798 | 82.913 | 93.305 |

The endpoint starts when device userspace reads the picture's first VCL byte and
ends after copying into camera YUV planes. It excludes host capture/encoding,
buffer unlock, app delivery, and display. Host/device clock uncertainty is around
120 ms in these comparisons, too large to isolate USB/ADB latency. Runs are
sequential samples on one phone, with instrumentation overhead and variable CPU
load; percentile differences are not additive or universal guarantees.

The original-scaler pairs isolate framing. FLV provides complete-picture lengths,
so the decoder can submit a picture without waiting for the next picture to mark
its end. The sender flushes FLV packets directly to the existing TCP socket;
there is no new desktop relay in normal operation. Raw Annex-B remains supported.
The profiler itself still uses its measurement proxy.

The supported FLV profile is deliberately narrow: AVC, one SPS/PPS, one complete
picture per tag, NAL length fields of 1, 2, or 4 bytes, no composition-time offset,
and stable configuration until reconnect. Bounds, trailers, and picture starts
are validated. Generic FLV can contain layouts outside this profile.

Scaling preserves the original fixed-point bilinear rounding. Crops requiring no
resize use row copies; other sizes cache horizontal interpolation and use NEON
for the vertical pass on ARM64. Here 1080p → 1440×1080 is a crop-only operation.
Differential tests cover 160 layouts, padding, pixel strides 1–4, and output
canaries. Standalone old/new checksums matched at both source sizes.

## Extended preview soak and ring reclamation

The first attempt completed 18 cycles before Camera closed its preview. That
attempt is incomplete, not a passing soak. Its stopped-state cameraserver RSS
rose from 215,448 to 332,896 KiB, exposing frame rings retained after each
injection. Historical retained rings require process restart to reclaim.

Shutdown now disables substitution and releases the ring mapping/fd when the last
in-flight callback exits. It never waits for a callback: the injector may have
borrowed that same thread. Late callbacks forward through the retained trampoline.
Payload images and trampolines remain mapped, so some per-injection overhead
remains. Failed initialization that never installed a hook cleans up synchronously
and remains eligible for retry.

The restarted soak passed **64/64 cycles in 524.58 seconds (8 minutes 45 seconds)**,
with 2,785 distinct live publications consumed. It alternated 720p/1080p and
covered disconnect, stop while live, pause/resume, and reconnect, with both file
and pattern fallback. Cameraserver PID stayed **23424**, all feeder exits were
zero, fatal-log checks passed, and all 64 rings logged reclamation. RSS went from
338,804 to 343,948 KiB (5.02 MiB net; peak 344,044 KiB), including old allocations
from earlier tests. This removes the large ring accumulation; it does not prove
zero growth or unlimited reinjection safety.

At the user's request, a temporary host process tapped preview coordinates
(540, 1000) every 20 seconds, only while `com.android.camera` was the resumed
activity. This kept the preview open without changing system timeout settings.

The soak used build `63ee56fcd2b82260`. A subsequent failed-initialization cleanup
correction produced `b0eeefb796f9e96a`; its 15-second 1080p check consumed 221
distinct publications at 29.99 decoded fps, with receive-to-copy p50/p95 of
64.575/87.922 ms. These short-run results are separate from the paired comparison.

## Idle injection uncovered borrowed-thread errno corruption

After the preview checks, an idle injection returned success but cameraserver
aborted immediately after detach, before the self-test began. The Binder log
reported `getAndExecuteCommand(fd=5) returned unexpected error -2`. PID changed
from 23424 to 22358. This failure is outside the successful preview soak.

Remote calls restored registers but left the borrowed thread's TLS `errno`
changed. A deterministic child-process regression reproduced corruption in
10/10 cases: a remote function changed `EINTR` to `ENOENT`, and resumed code
observed the latter. The injector now obtains the address using Bionic's
`__errno`, saves its value before remote work, and restores it before original
code runs toward a syscall stop or detach. The accessor returns the current
thread's errno storage. [Bionic implementation](https://android.googlesource.com/platform/bionic/+/refs/heads/main/libc/bionic/__errno.cpp)

The fixed regression passed 10/10. Actual injection logs show errno restored
from **2 to 4**, matching the observed Binder failure mechanism. Final build
`8657d68133075843` passed **20/20 idle inject/stop cycles**, including three
**63/63** self-test runs. PID remained 22358 throughout these repetitions. A final eight-cycle live run
with the same build also passed all file/pattern and mixed-resolution cases
(382 distinct live publications consumed; 64.24 seconds), with no
fatal logs or PID change. The tapper was stopped afterward. Final device state
has no feeder, active hook, or frame ring.

## Other validation and reproduction

ARM64 and x86_64 production builds passed. Native decoder scheduling, FLV
fragmentation/backpressure/EOS, lifecycle, concurrent hook retirement, and 100
ring create/destroy cycles passed. Host framing/scaling sanitizer checks and
six desktop plus six benchmark-analysis tests passed. Runtime coverage is one
physical ARM64 device; x86_64 and other vendors/Android releases remain unverified.

```sh
python3 tools/profile_latency.py --serial DEVICE_SERIAL --seconds 25 \
  --size 1280x720 --transport flv --output .scratch/transport/new-flv
python3 tools/profile_latency.py --serial DEVICE_SERIAL --seconds 25 \
  --size 1280x720 --transport annexb --output .scratch/transport/new-annexb
python3 tools/stress_lifecycle.py --serial DEVICE_SERIAL --cycles 64 \
  --alternate-sizes --output .scratch/transport/new-soak
```

Keep a live preview open and start with ReCam stopped. Every output directory
must be new. The benchmark does not automatically tap the screen. Native test
commands are in [android/tests/README.md](../android/tests/README.md).

[Machine-readable results](transport-2026-11-08.json) retain comparison metrics,
both soak outcomes, and idle regression results. Local raw logs are under
`.scratch/transport/`, especially the six named comparison directories,
`soak-64/`, `soak-reclaim-64/`, `final-cleanup-build/`, `idle-errno.log/.json`,
`remote-errno-before.txt`, `remote-errno-after.txt`, and `final-errno-live/`. Earlier profiler fields
named `final_status` describe the measurement endpoint before cleanup; the soak
and idle-test final states verify actual cleanup separately.

The [extended validation pass](extended-2026-11-08.md) adds a 30-minute phone soak,
API 31 emulator coverage, fresh-inode staging for file-route reinjection, and
source-to-compositor timing bounds.
