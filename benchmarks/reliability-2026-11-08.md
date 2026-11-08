# Slot ownership and resident reuse — 2026-11-08

This pass addresses the ring-overrun fallback and repeated-injection mapping
growth recorded in [extended validation](extended-2026-11-08.md). The final staged
and deployed build is **24c394dd11ca7c8e**. Stream validation used **90e5b2c55f60f228**;
the final change only caches self-test trampolines. Tests used the POCO Android 16
ARM64 phone; cameraserver PID **22358** remained unchanged.

## Implementation

Previously, a camera callback copied the latest slot without reserving it. If
the producer advanced three publications during that copy, the callback detected
the lap afterward and filled the output with color bars. That check also happened
after concurrent pixel reads/writes had already occurred.

Ring version **4** pins slots with shared reader ownership. The single producer
claims an unpinned slot other than the current publication. If both alternatives
are pinned, it skips that decoded publication and preserves the complete current
frame. `ring.producer_skipped` counts this condition. Sequence gaps identify
skipped slots; they are not counts of dropped compressed input. Per-slot sequence
and timestamp metadata stay stable while copying. Aborted writes invalidate the
slot sequence before releasing ownership.

Lane metadata has separate ownership so reclamation or geometry changes cannot
race a camera copy. A ring-fd `flock` excludes competing producers and releases
automatically on process death. The next producer clears abandoned writer tokens
and invalidates incomplete slots, preserving camera reader counts. Generation
checks detect ring replacement even when the fd number is reused.

Mapping growth is addressed by **same-build reuse**, not by unloading code. The
existing v2 control-page prefix remains unchanged; an optional extension identifies
the resident payload by GNU ELF build ID, generation, base, and initialization
entry point. Once callback references have retired and the ring is destroyed,
the injector can initialize that image again. Its original trampoline is immutable
and reused. A late callback that entered before removal can still forward safely.
Busy retirement returns a retry result; the injector resumes the process between
bounded attempts, allowing interrupted callbacks to complete.

`dlclose` remains unsafe: a callback counter cannot detect a thread that has already
branched toward the payload but has not incremented the counter. Different builds
therefore load fresh images. Older images remain until camera-service restart.
No restart was performed to clear the old mappings during this validation.

## Validation

- Host ASan/UBSan and ThreadSanitizer passed the slot-ownership test. It holds a
  frame across 10,000 newer publications, saturates all slots, aborts a write, and
  verifies concurrent copies from four readers during 100,000 publication attempts.
- Android ARM64 passed that test plus a real memfd/process-death test: a competing
  file lock is rejected, a child producer is killed while holding write ownership,
  and recovery preserves the surviving reader's slot and timestamp.
- Actual ARM64 hooks passed **3,000** install/remove cycles across plain, BTI, and
  PAC prologues. Each target retained one trampoline address; both hooked behavior
  and original calls after removal remained correct.
- Hook shutdown tests passed **100** retirement/restart transitions, including
  active-callback refusal and late disabled callbacks during preparation. Payload
  lifecycle tests passed **23** checks. GNU note parsing/build-ID matching passed.
- ARM64 and x86_64 builds passed without new compiler warnings. The 12 tool tests
  and 6 desktop tests passed. x86_64 runtime and Android 12 were not rerun here.

The first phone check completed **30 start/stop cycles** using one payload base,
**4,156 VMAs**, and **106,352 KiB** stopped-state RSS throughout. Logcat recorded
29 resident reuses after the initial load. The harness initially searched stdout
for those logs and failed its logging assertion. The original report is retained;
separate analysis checks the actual logcat records and unchanged base/map counts.

The stream-validation build completed another **10 starts** at one payload base with
**4,277 VMAs** throughout. Stopped-state RSS ranged from **105,956 to 106,212 KiB**
(256 KiB variation). The preview tapper was active. This demonstrates no new image
mapping per start; it does not prove total camera-process memory is constant.
Absolute VMA counts across different test phases are not comparable because live
preview buffers and multiple development builds were introduced between them.

The mixed 720p/1080p live run passed **12/12 cycles in 100.80 seconds**, covering
disconnect, stop while live, pause/resume, reconnect, and file/pattern fallback.
It used build `c657ea82d4340481`, before aborted-slot invalidation was added. The
subsequent feeder restart test used `b4fb204f7705cf4f`: **10 SIGKILL/restart cycles**
passed, a second producer exited with status 1, and reinjection while feeding
advanced generation 289 to 290 and resumed publication. The stream-validation build factors
that same recovery logic into the shared helper tested by `ring_restart.cpp`.

Stream-validation screenshot stress matched **30/30 distinct source frames at 720p**
and **30/30 at 1080p**, with no color-bar sample or offline recovery. Both runs
reported zero ring drops and zero producer skips; saturation behavior is covered
by the deterministic ownership test rather than claimed from these live runs.
The temporary foreground-guarded tapper ran every 20 seconds during testing.

| Source | Median source-frame-age bounds | p95 bounds |
|---|---:|---:|
| 720p | 0–183.99 ms | 0–258.96 ms |
| 1080p | 0–202.36 ms | 0–263.26 ms |

These are compositor-snapshot age bounds from the existing measurement tool,
not optical latency or a controlled performance comparison. The two-second stale
source fallback and fallback for unserved/unsupported geometry remain intentional.
Bounded nonblocking acquisition can still decline under contention; these changes
remove overwrite-induced tearing rather than guarantee zero fallback in all cases.

Final review found that repeated `recam test` calls also consumed fresh trampoline
slots. Each self-test target now retains its hook record and reuses its trampoline.
The final deployed build passed **45 consecutive full self-tests** in one resident
image, beyond the previous arena-exhaustion threshold, with the same camera PID.
The Camera app was closed for these tests and reopened afterward.

## Evidence and cleanup

[Machine-readable report](reliability-2026-11-08.json) includes the initial and final
reuse checks, original harness failure, live cycles, feeder restarts, and both
complete compositor datasets. Raw logs, screenshots, and source timestamps remain
under `.scratch/reliability/`. The native test source and commands are documented
in [android/tests/README.md](../android/tests/README.md).

Testing finished with the tapper stopped, no active hooks, no feeder or frame ring,
and no ADB forwards. The reusable resident image and older retained images remain
mapped as described above.
