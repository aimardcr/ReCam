# Live scheduling comparison — 2026-11-08

Same device, source settings, 25-second runs, 1440×1080 stock-camera lane, and
measurement endpoints as the [baseline](latency-2026-11-08.md). First three seconds
and the nominal final second are excluded. Times below are **device userspace
receive → completed copy into camera YUV planes**, not capture-to-display latency.

| Source | Baseline p50 | Updated p50 | Baseline p95 | Updated p95 | Updated p99 |
|---|---:|---:|---:|---:|---:|
| 720p30 | 3270.76 ms | 95.00 ms | 4749.82 ms | 126.27 ms | 153.88 ms |
| 1080p30 | 3000.22 ms | 100.74 ms | 4446.73 ms | 124.90 ms | 143.97 ms |

The updated decoder sustained **29.98 fps** in both runs. Publication rate was
**29.64 / 29.50 fps**: older decoded outputs can be superseded when several are
ready, so publication rate is intentionally distinct from decode throughput.
There were 840 / 845 matched consumption samples, using 531 / 512 unique
publications. Repeated camera consumption of a frame is included in frame age.

Receive-to-publication median in the first versus last three-second buckets:

- 720p: **76.40 → 76.65 ms** (baseline **1654.59 → 4664.28 ms**).
- 1080p: **80.13 → 76.94 ms** (baseline **1583.86 → 4373.43 ms**).

The growing backlog was absent in these runs. This is evidence for this device and
workload, not a universal latency or throughput guarantee. An earlier valid pair
measured 106 / 117 ms median and 157 / 147 ms p95, illustrating run-to-run variation.

## Changes

- Drain ready codec outputs before input and around each complete-picture
  submission; live input and output dequeues are nonblocking.
- Retain the newest valid decoded output and promptly release every codec buffer.
- Service live input independently of the file playback tick. Socket `poll()`
  waits up to 2 ms before rechecking the synchronous codec API; OS scheduling can
  delay execution further.
- Preserve the last publication during short gaps. After two seconds without a
  new live frame, resume the paced file/pattern fallback. Disconnect restores the
  fallback too.
- Keep decoder-owned frame storage alive until scaling finishes, including an
  iteration that observes EOF and returns a frame together.
- Close a connection on a failed/oversized encoded-picture submission or an
  excessive compressed backlog instead of skipping dependent reference data.

Transport, Annex-B picture framing, encoder settings, and scaling are unchanged.
The final build also preserves the configured fallback interval while the live
sender is stalled. This last fallback-only adjustment was tested separately and
does not affect the healthy-live path measured above.

## Remaining stages

| Median stage | 720p | 1080p |
|---|---:|---:|
| Receive → codec submission | 36.82 ms | 34.71 ms |
| Submission → output dequeue | 20.24 ms | 24.59 ms |
| Output dequeue → CPU copy complete | 0.76 ms | 1.18 ms |
| Scaling | 13.41 ms | 12.51 ms |
| Publication → camera copy start | 16.62 ms | 16.64 ms |
| Camera-plane copy | 1.69 ms | 1.63 ms |

Stage percentiles cannot be added. The remaining pre-submission delay is
consistent with the existing Annex-B lookahead; explicit access-unit boundaries
are a candidate for the next isolated experiment. Codec timing includes polling
and buffering, not just hardware decode work.

Host/device clock uncertainty remained about ±121–130 ms, comparable to or larger
than the improved device latency. Host-derived midpoint estimates therefore are
not useful as precise end-to-end figures. Capture/encoding, buffer unlock, app
delivery, and display remain outside the measured device path.

## Validation and evidence

- ARM64 and x86_64 builds passed.
- Deterministic native codec tests cover draining before submission, newest-frame
  selection, preservation of timing after invalid output, nonblocking live
  dequeues, and termination when reference-picture submission fails.
- Actual file/live switching passed: a 15 fps file published about 14.25 fps;
  live input about 28.89 fps; a short pause produced zero new publications;
  stalled-file fallback about 14.18 fps; resumed live about 31.09 fps during
  catch-up; disconnected-file fallback about 14.14 fps. Short status-sampling
  windows and decode/scale work affect these observed rates.
- Receive-timeline, profile-analysis, and desktop lifecycle regression tests pass.
- A final shutdown check exposed a null source-pointer dereference in the hook
  (`recam_on_return_buffer`, old image offset `0x5698`). Removal cleared the source
  pointers while an in-flight callback could still use them. The final build
  retains the static source pointers, matching the existing trampoline lifetime.
  The deterministic in-flight shutdown regression passes, followed by five
  inject/stop cycles with active preview (80–102 substituted buffers per cycle),
  unchanged cameraserver PID 23424, and inactive payload state after every stop.
- All 63 in-process self-tests passed with the camera preview closed. An earlier
  invocation during active preview returned failure and coincided with a daemon
  restart without a new tombstone. Its cause is unresolved; do not run this test
  concurrently with preview or feeding.
- The final device state has no active hook or feeder. The shutdown-only change
  does not alter the healthy-live scheduling path measured above.

[Complete statistics, decoder counters, and clock samples](scheduling-2026-11-08.json).
Raw local evidence is under `.scratch/latency/scheduled-final-720p-active/` and
`.scratch/latency/scheduled-final-1080p-active/`; switching results are in
`.scratch/latency/switching.json`. Runs started without a camera preview are
excluded. The earlier valid pair is under `scheduled-720p-active/` and
`scheduled-1080p-active/`.

## Follow-up reliability work

The subsequent [stability pass](stability-2026-11-08.md) reproduced the feeder
abort in 7/30 live-to-pattern teardown cycles and captured two tombstones. The
fault was in Android's detached buffer-pool eviction thread while the main thread
ran exit destructors. Explicit feeder cleanup followed by `_exit()` avoided all
aborts in 30 matched repeat cycles; eight additional preview/switching cycles also
passed. The self-test CLI now rejects active camera clients and feeders before
attachment. The exact earlier self-test restart was not reproduced or attributed;
its unsafe live invocation is now rejected.

Shutdown verification is retained locally in `shutdown-validation.json/.log` and
`selftest-final.log` under `.scratch/latency/`. The symbolized shutdown crash is
`tombstone-stop.txt`. Final deployed build identifier: `bd5c020d05221f94`.
