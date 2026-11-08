#!/usr/bin/env python3
"""Bound synthetic source-frame age in Android compositor screenshots.

Requires Pillow, qrcode, opencv-python-headless, adb, ffmpeg and a live preview.
This is NOT an optical capture-to-display measurement. Request start and first
PNG-header arrival bracket sampling; no clock synchronization is assumed.
"""
import argparse
import json
import math
from pathlib import Path
import re
import subprocess
import sys
import threading
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "desktop"))
from recam_ui.device import Device, STREAM_PORT
from recam_ui.streamer import build_command


def age_bounds(generated_ns, request_ns, sampled_upper_ns, reply_ns=None):
    reply_ns = sampled_upper_ns if reply_ns is None else reply_ns
    if not generated_ns <= sampled_upper_ns <= reply_ns or request_ns > sampled_upper_ns:
        raise ValueError("inconsistent frame or screenshot timestamps")
    return {"lower_ms": max(0, request_ns - generated_ns) / 1e6,
            "upper_ms": (sampled_upper_ns - generated_ns) / 1e6,
            "screenshot_round_trip_ms": (reply_ns - request_ns) / 1e6}



def screenshot(device):
    # screencap captures/locks the image before emitting PNG bytes. The signature
    # arrival bounds sampling without including the remaining encoding/transfer.
    result, errors = {}, []
    request = time.monotonic_ns()
    proc = subprocess.Popen(device._argv(["exec-out", "screencap", "-p"]),
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE, bufsize=0)

    def receive():
        try:
            header = b""
            while len(header) < 8:
                chunk = proc.stdout.read(8 - len(header))
                if not chunk:
                    raise RuntimeError("Truncated screenshot header")
                header += chunk
            result["sampled_upper_ns"] = time.monotonic_ns()
            if header != b"\x89PNG\r\n\x1a\n":
                raise RuntimeError("Invalid screenshot PNG signature")
            result["data"] = header + proc.stdout.read()
            result["reply_ns"] = time.monotonic_ns()
        except Exception as exc:
            errors.append(exc)

    reader = threading.Thread(target=receive, daemon=True)
    reader.start()
    try:
        reader.join(timeout=15)
        if reader.is_alive():
            raise TimeoutError("Screenshot timed out")
        if errors:
            raise errors[0]
        if proc.wait(timeout=5):
            raise RuntimeError(proc.stderr.read().decode(errors="replace"))
        return result["data"], request, result["sampled_upper_ns"], result["reply_ns"]
    finally:
        if proc.poll() is None:
            proc.kill()
            proc.wait()
        reader.join(timeout=5)
        proc.stdout.close()
        proc.stderr.close()



def decode_frame(image, detector, cv2):
    # Large, perfectly aligned codes can confuse finder detection at one scale.
    # All retries use the same saved pixels; timing remains the capture timing.
    for scale in (1, .5, .25):
        candidate = image if scale == 1 else cv2.resize(image, None, fx=scale, fy=scale)
        value, _, _ = detector.detectAndDecode(candidate)
        if value:
            return value
    value, _, _ = detector.detectAndDecode(cv2.flip(image, 1))
    return value


def distribution(values):
    values = sorted(values)
    if not values:
        return {"count": 0}
    return {"count": len(values), "p50": values[math.ceil(.5 * len(values)) - 1],
            "p95": values[math.ceil(.95 * len(values)) - 1], "max": values[-1]}


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--serial", required=True)
    ap.add_argument("--output", type=Path, required=True)
    ap.add_argument("--samples", type=int, default=30)
    ap.add_argument("--size", default="1280x720")
    ap.add_argument("--adb-root", action="store_true", help="root adbd, for userdebug emulators")
    args = ap.parse_args()
    if args.samples < 1 or not re.fullmatch(r"[1-9]\d*x[1-9]\d*", args.size):
        ap.error("use positive samples and WIDTHxHEIGHT")
    width, height = map(int, args.size.split("x"))
    if min(width, height) < 240 or max(width, height) > 1920 or width % 2 or height % 2:
        ap.error("use even dimensions between 240 and 1920")
    # Optional dependencies are confined to this measurement tool.
    import cv2
    import numpy as np
    import qrcode
    from PIL import Image, ImageDraw

    class BenchmarkDevice(Device):
        def shell(self, cmd, root=True, timeout=30):
            return super().shell(cmd, root=root and not args.adb_root, timeout=timeout)

    args.output.mkdir(parents=True, exist_ok=False)
    d = BenchmarkDevice(args.serial)
    initial = d.status()
    if not initial or initial["feeder"]["running"] or initial["payload"].get("active"):
        raise RuntimeError("Stop ReCam before measuring")
    report = {"initial": initial, "size": args.size, "requested_fps": 30, "samples": [],
              "endpoint": "synthetic frame generation to compositor screenshot sampling",
              "excluded": ["physical source capture", "physical display scanout and response"],
              "sampling_bound": "request start to first PNG signature received",
              "caveats": ["snapshot frame age is not first-presentation latency",
                          "screencap perturbs the device; header arrival is only an upper bound",
                          "a synthetic QR scene is not representative camera content"]}
    encoder = producer = None
    forwarded = None
    injected = False
    stop = threading.Event()
    timestamps, producer_errors = {}, []
    started_ns = None
    try:
        report["build"] = d.ensure_deployed()
        injected = True
        d.inject()
        deadline = time.monotonic() + 10
        while not (d.status() or {}).get("ring", {}).get("lanes"):
            if time.monotonic() > deadline:
                raise RuntimeError("No active camera buffers; open an unlocked live preview")
            time.sleep(.2)
        d.start_feeder()
        rc, out, err = d._run(["forward", "tcp:0", f"tcp:{STREAM_PORT}"])
        if rc:
            raise RuntimeError(err)
        forwarded = int(out)
        time.sleep(.5)
        command = build_command("ffmpeg", ["-f", "rawvideo", "-pixel_format", "rgb24",
                                "-video_size", args.size, "-framerate", "30", "-i", "pipe:0"], forwarded)
        report["command"] = command
        with (args.output / "ffmpeg.log").open("w") as log:
            encoder = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.DEVNULL,
                                       stderr=log)

            def produce():
                try:
                    frame = 0
                    deadline = time.monotonic()
                    while not stop.is_set():
                        generated = time.monotonic_ns()
                        code = qrcode.QRCode(version=1, error_correction=qrcode.constants.ERROR_CORRECT_M,
                                             box_size=1, border=4)
                        code.add_data(f"RC{frame:08X}")
                        code.make(fit=False)
                        qr = code.make_image(fill_color="black", back_color="white").convert("RGB")
                        side = (min(width, height) * 3 // 4 // qr.width) * qr.width
                        qr = qr.resize((side, side), Image.Resampling.NEAREST)
                        image = Image.new("RGB", (width, height), (96, 96, 96))
                        image.paste(qr, ((width - side) // 2, (height - side) // 2))
                        ImageDraw.Draw(image).text((width // 3, 10), f"ReCam frame {frame}", fill="white")
                        timestamps[frame] = generated
                        encoder.stdin.write(image.tobytes())
                        encoder.stdin.flush()
                        frame += 1
                        deadline = max(deadline + 1 / 30, time.monotonic())
                        stop.wait(max(0, deadline - time.monotonic()))
                except Exception as exc:
                    if not stop.is_set():
                        producer_errors.append(str(exc))

            started_ns = time.monotonic_ns()
            producer = threading.Thread(target=produce, daemon=True)
            producer.start()
            time.sleep(3)
            detector = cv2.QRCodeDetector()
            for sample in range(args.samples):
                if producer_errors or encoder.poll() is not None:
                    raise RuntimeError(f"Source failed: {producer_errors}; see ffmpeg.log")
                data, request, sampled_upper, reply = screenshot(d)
                filename = f"screenshot-{sample:03d}.png"
                (args.output / filename).write_bytes(data)
                image = cv2.imdecode(np.frombuffer(data, dtype=np.uint8), cv2.IMREAD_COLOR)
                value = ""
                if image is not None:
                    value = decode_frame(image, detector, cv2)
                row = {"screenshot": filename, "request_ns": request, "reply_ns": reply,
                       "sampled_upper_ns": sampled_upper, "decoded": value}
                if re.fullmatch(r"RC[0-9A-F]{8}", value):
                    frame = int(value[2:], 16)
                    if frame in timestamps:
                        row.update(frame=frame, generated_ns=timestamps[frame],
                                   **age_bounds(timestamps[frame], request, sampled_upper, reply))
                report["samples"].append(row)
                print(json.dumps(row), flush=True)
                time.sleep(.2)
            report["source_frames"] = len(timestamps)
            report["source_fps"] = len(timestamps) * 1e9 / (time.monotonic_ns() - started_ns)
            for metric in ("lower_ms", "upper_ms", "screenshot_round_trip_ms"):
                report[metric] = distribution(r[metric] for r in report["samples"] if metric in r)
            report["unique_frames"] = len({row["frame"] for row in report["samples"] if "frame" in row})
            report["measurement_end"] = d.status()
            report["passed"] = (all("frame" in row for row in report["samples"]) and
                                report["unique_frames"] >= min(2, args.samples) and
                                report["measurement_end"] is not None and
                                report["measurement_end"]["cameraserver"]["pid"] == initial["cameraserver"]["pid"])
            if not report["passed"]:
                raise RuntimeError("Missing or stagnant frame IDs, or camera PID changed; inspect saved images")
    except Exception as exc:
        report.update(passed=False, error=str(exc))
        raise
    finally:
        stop.set()
        errors = []

        def cleanup(action):
            try:
                action()
            except Exception as exc:
                errors.append(str(exc))

        if encoder:
            if encoder.poll() is None:
                encoder.terminate()
                try:
                    encoder.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    encoder.kill()
                    encoder.wait()
            if producer:
                producer.join(timeout=5)
            if encoder.stdin:
                cleanup(encoder.stdin.close)
        if injected:
            cleanup(d.stop_feeder)
            cleanup(d.stop_payload)
        if forwarded:
            cleanup(lambda: d.remove_forward(forwarded))
        cleanup(lambda: report.update(final=d.status()))
        if errors:
            report.update(passed=False, cleanup_errors=errors)
        (args.output / "source_frames.json").write_text(json.dumps(timestamps) + "\n")
        (args.output / "report.json").write_text(json.dumps(report, indent=2) + "\n")
        if errors:
            raise RuntimeError(f"Cleanup failures: {errors}")


if __name__ == "__main__":
    main()
