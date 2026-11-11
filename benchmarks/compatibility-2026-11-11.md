# Camera compatibility validation — 2026-11-11

This pass addresses the resolution limit reported in [issue #2](https://github.com/aimardcr/ReCam/issues/2)
and improves compatibility diagnostics. It does **not** claim Qualcomm UBWC
recording support or a verified fix for the OnePlus selfie corruption.

## Implementation

- Ring protocol 5 distinguishes requested camera geometry from stored geometry.
- The ring allocation stays at 37,605,376 bytes (35.9 MiB), with four triple-buffered
  lanes. Stored frames remain bounded by 1920×1088. Camera output is bounded by
  8192 pixels per dimension and 33,554,432 pixels total, with even dimensions.
- Larger outputs are bilinearly enlarged into gralloc's reported plane strides.
  This consumes additional camera-thread time and does not add native image detail.
  Existing in-budget output uses the direct-copy path.
- Destination plane strides and dimensions are validated before writes. Color-bar
  rows are local to each callback, removing a shared cache race across geometries.
- Status and UI expose requested/stored sizes, scaling, stopped/waiting/stale
  states, and cumulative fallback and compatibility counters. Per-stream logs
  include lock API, plane strides and chroma pointer separation, with repeat
  logging limited to once per five seconds for a cached observation.

## Validation

- Android ARM64 and x86_64 payload/CLI cross-builds passed with NDK 27.3.13750724.
- Host large-frame tests passed with AddressSanitizer + UndefinedBehaviorSanitizer,
  and separately ThreadSanitizer. They cover all reported sizes, portrait outputs,
  the 8192×4096 boundary, 122 bilinear differential cases, planar/interleaved/padded
  planes, destination guards, invalid layouts and concurrent color-bar writers.
- Existing ring ownership and in-flight hook shutdown checks passed under ASan/UBSan.
- The large-frame component suite passed on the connected POCO 2409FPCC4G,
  Android 16 / API 36. The injected `recam test` command also returned success
  before live camera testing.
- Nine desktop tests and twelve tooling tests passed. The diagnostics panel was
  rendered offscreen with PySide6 6.12.0 and inspected for clipping/wrapping.

The real feeder served four synthetic requests using the production consumer in
a separate process, with no camera clients. Moving pixels and destination guards
were checked; all lanes reported `served` and `scaled`, with zero drops:

| Requested camera size | Stored feed size | One measured copy |
| --- | --- | --- |
| 1960×882 | 1920×864 | 24.841 ms |
| 4608×3456 | 1450×1088 | 34.271 ms |
| 4000×3000 | 1450×1088 | 25.698 ms |
| 1080×1920 | 612×1088 | 10.087 ms |

These are individual standalone copy timings, subject to scheduling and CPU state;
they are neither throughput benchmarks nor capture-to-display latency measurements.
This exercise does not validate large vendor gralloc buffers themselves.

The **current build** passed eight live Camera cycles at alternating 720p/1080p
input, covering disconnect, stopping during a live stream, pause/resume, reconnect,
and file/live switching. There were 334 matched live publications consumed, no
detected crashes, and cameraserver stayed at PID 22358. Native preview outputs
were 1440×1080 YUV_420_888 and YV12. Ring version 5 and the new per-stream logs were
verified during this run. All feeder exits were zero. The preliminary run that
selected an older staged desktop payload was excluded from these results.

Pause tests intentionally retain old frames for about two seconds, so their age
statistics must not be interpreted as steady-state transport latency. This pass
makes no latency improvement claim.

Final device state: feeder stopped, hooks removed, no live ring, periodic tapper
stopped. Existing source video was preserved. Detailed results and binary hashes
are in [the JSON report](compatibility-2026-11-11.json).

## Remaining validation

The reporter must test the OnePlus 9 again. Its Qualcomm UBWC recording path
remains unsupported; JPEG/BLOB and RAW buffers remain pass-through. The observed
selfie stripes may involve layout or downstream processing beyond the cache race
fixed here. No saved-photo or saved-video compatibility claim is made from the
POCO preview tests.
