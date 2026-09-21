# ReCam desktop console

A PySide6 front end for the on-device tools. It spawns `adb` and `ffmpeg`; it speaks no
protocol of its own. The device-side surface it drives is described in
[`../docs/11-ui-interface.md`](../docs/11-ui-interface.md).

```bash
cd desktop
pip install -r requirements.txt
python main.py
```

Needs `adb` and `ffmpeg` on PATH and a rooted device with USB debugging.

The device binaries are found in this order: a frozen PyInstaller bundle, then
`recam_ui/payload/arm64-v8a`, then `android/libs/arm64-v8a` from an `ndk-build`. So a
release ships self-contained, and a checkout works straight from the build tree.

## Building a release

```bash
set ANDROID_NDK_HOME=...
dk.3.13750724
python tools/package.py
```

That runs `ndk-build`, copies the six device binaries into the package, and freezes it
with PyInstaller to `dist/ReCam`. `--no-ndk` reuses `android/libs/arm64-v8a` as it stands;
`--no-freeze` stages the payload without freezing.

`adb` and `ffmpeg` are **not** bundled — they stay external dependencies on PATH.

## Using it

Pick a device, pick a source, press **Start**. There is no separate deploy step: Start
pushes the device binaries when their content hash differs from what is already there,
so a rebuild is picked up automatically and an unchanged one costs nothing.

**Stop** reverses everything and restores the real camera without restarting
cameraserver.

| Source | What happens |
|---|---|
| Test pattern | The feeder generates scrolling bars with a moving marker |
| RTMP | ffmpeg itself listens; publish to the Server and Stream Key shown beside the radio button |
| Video file | The chosen file is pushed to `/data/local/tmp/recam_source.mp4` and looped on-device |
| FFmpeg | A path, a URL, or raw input flags if the text starts with `-` |
| Camera | Any DirectShow capture device, including **OBS Virtual Camera**. The list refreshes on launch and whenever you click the row |

**Video file** is the most robust mode: once pushed, nothing depends on the desktop
staying alive. The others all run an ffmpeg child here and stream over `adb forward`.

## Feeding OBS in

Two routes, both verified:

**OBS Virtual Camera** — in OBS press *Start Virtual Camera*, then pick
`OBS Virtual Camera` in the **Camera** dropdown here and press Start. Simplest, and no
encoder settings to get wrong.

**OBS streaming over RTMP** — select **RTMP** here and press Start first, so ffmpeg is
listening. Then in OBS, *Settings -> Stream -> Service: Custom*, and copy the Server and
Stream Key shown next to the radio button. The Stream Key must match exactly: ffmpeg's
listener rejects any other. Set *Output -> Encoder -> B-frames* to **0**.

RTMP is worth the extra steps only if you want OBS's own encoder settings to reach the
device; otherwise the virtual camera is less to go wrong.

## Two things it does deliberately

**`-bf 0` is always passed to ffmpeg.** The device synthesises timestamps in decode
order, so B-frames would reorder the output — see
[`../docs/10-live-stream.md`](../docs/10-live-stream.md) open item 6. This is why no
MPEG-TS wire format is needed. It does **not** cover RTMP, where OBS does its own
encoding: set B-frames to 0 there yourself.

**`root: false` is reported as "cannot determine", not "not injected".** Without root
the status reads fail, and an uninjected process is indistinguishable from one the tool
cannot see into.

## State panel

Polled once a second from `recam_status`. `Rate` is computed by differencing the
cumulative `published` counter across polls. `consumed` exceeding `published` is normal:
two camera streams of the same size share one lane.
