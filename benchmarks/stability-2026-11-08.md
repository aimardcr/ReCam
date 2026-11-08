# Lifecycle stability — 2026-11-08

Device: POCO 2409FPCC4G, Android 16 / API 36, ARM64. Final deployed build:
`7f4f3b14221e6b91`. This pass follows the [scheduling comparison](scheduling-2026-11-08.md);
it changes process exit and self-test admission, not healthy-stream transport.

## Feeder exit failure

Thirty live-to-pattern teardown cycles reproduced **seven** feeder aborts. Every
invocation nevertheless returned **exit code 0**. The fatal background-thread
message can race the main thread's successful process exit, so checking only the
ADB return code misses this failure.

Two tombstones show the failing stack:

```text
pthread_mutex_lock
std::__1::mutex::lock
Accessor::evictorThread
  /system/lib64/libstagefright_aidl_bufferpool2.so
```

The main thread in the same captured process was in `__cxa_finalize` / `exit`.
The thread's inherited name was `CodecLooper`, but the faulting implementation
was the buffer-pool eviction worker. Android's corresponding implementation
creates that worker with references to an `AccessorEvictor`'s mutex and condition
variable, detaches the thread, and owns the object through a static `unique_ptr`.
There is no termination/join path for that worker. This matches the observed race
between static destruction and the detached worker.
[Android 16 Accessor implementation](https://android.googlesource.com/platform/hardware/interfaces/+/android16-release/media/bufferpool/aidl/default/Accessor.cpp)

The ReCam workaround is confined to the `feed` command: return from its explicit
codec/socket/mapping cleanup, flush buffered diagnostics, then call `_exit(rc)`.
This avoids platform static destruction while terminating all process threads.
Other subcommands keep ordinary exit behavior. It does not change per-connection
codec cleanup or attempt to modify Android's library. During review, a separate
format-string lifetime bug was fixed: file-decoder logging now uses the MIME
string before deleting its owning `AMediaFormat`.

## Self-test admission

The original preview-time self-test restart did not reproduce in one captured
retry; its exact cause remains unproven. Inspection confirmed that the test
rewrites live shared-ring metadata and invokes gralloc on the borrowed main
thread. It is unsuitable for concurrent camera or feeder operation.

`recam test` now rejects a running feeder or any active camera client before
ptrace attachment. Camera state comes from a three-second `dumpsys media.camera`
query; failed, oversized, ambiguous or unsupported output is rejected. Both live
rejection paths were exercised on the device. With preview closed and feeder
stopped, all **63 checks passed**, without a cameraserver restart.

This is a preflight snapshot. Keep clients stopped throughout the test; it does
not prevent an unrelated app or command from opening a camera or starting a
feeder after the snapshot.

## Validation

| Check | Result |
|---|---|
| Matched live-to-pattern teardown, before workaround | 7 fatal processes / 30 cycles; all returned 0 |
| Same teardown after workaround | 0 fatal processes / 30 cycles |
| Preview start/stop and switching | 8 / 8 cycles passed; 432 distinct live publications consumed in total |
| `cameraserver` PID across final preview cycles | 23424 → 23424 |
| Short paused input | No new publications after buffered frames drained |
| Two-second stall fallback | Pattern and file both resumed publication |
| Reconnect / stop with live socket / EOF | Passed with both fallback sources |
| Idle in-process self-test | 63 passed, 0 failed |
| Native CLI-exit and preflight tests | Passed on host and ARM64 device |
| Python profile/crash-detection and desktop tests | 6 + 3 passed |
| Production builds | ARM64 and x86_64 passed; x86_64 not run on device |

The final device has no feeder or active hook. These are short tests on one
device, not proof of universal or long-duration reliability. The earlier
in-flight hook shutdown fix is also included in this deployed build.

## Reproduction and evidence

`tools/stress_lifecycle.py` runs the final preview scenarios with generated
720p30 H.264 and a 320×240 15 fps fallback file. It checks fatal log entries by
PID, actual matched live publication/consumption, process exit, hook state and
stable cameraserver PID. It retains partial results if a cycle fails.

```sh
python3 tools/stress_lifecycle.py --serial DEVICE_SERIAL --cycles 8 \
  --output .scratch/stability/run
```

Start with no active ReCam session and keep an unlocked camera preview open.
The output directory must be new. `adb` and `ffmpeg` are required.

[Machine-readable results](stability-2026-11-08.json) accompany this report. Local
raw evidence is in `.scratch/stability/`: `stress-pattern.log/.json`,
`stress-pattern-fixed.log/.json`, `codec-tombstone.txt`, `final/device.log`,
`final/report.json`, and `selftest.log/.json`. The initial file-only and short
standalone-decoder attempts did not reproduce the abort; they are not counted in
the matched 30-cycle comparison.

Follow-up: [framing, scaling, and extended-soak report](transport-2026-11-08.md)
records deferred ring reclamation and a separately observed idle-injection Binder
abort, its borrowed-thread errno fix, and the final preview/idle validation.
