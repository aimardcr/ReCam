#!/usr/bin/env python3
"""ADB lifecycle soak with actual codec output and crash-log verification.

Requires an idle ReCam session, ffmpeg, and an unlocked live camera preview.
Raw logs and partial results are retained even on failure. No third-party Python
packages are needed. Each cycle injects, feeds, switches, stops and checks the PID.
"""
import argparse
import json
import itertools
import math
from pathlib import Path
import re
import signal
import subprocess
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "desktop"))
from recam_ui.device import Device, STREAM_PORT
from recam_ui.streamer import build_command


def crash_lines(log, pids):
    result = []
    for line in log.splitlines():
        match = re.match(r"\S+\s+\S+\s+(\d+)\s+\d+\s+[A-Z]\s+", line)
        if match and int(match[1]) in pids and (
                "Fatal signal" in line or "FORTIFY:" in line):
            result.append(line)
    return result


def frame_ages(log):
    """Receive-to-camera-copy age, including repeated frames during pauses."""
    publications, consumes = {}, []
    for line in log.splitlines():
        match = re.search(r"event=(publish|consume)\s+(.*)", line)
        if not match:
            continue
        values = {k: int(v) for k, v in re.findall(r"(\w+)=(\d+)", match[2])}
        if not all(k in values for k in ("lane", "seq")):
            continue
        key = values["lane"], values["seq"]
        if match[1] == "publish" and values.get("receive", 0) > 0:
            publications[key] = values["receive"]
        elif match[1] == "consume" and "end" in values:
            consumes.append((key, values["end"]))
    ages = sorted((end - publications[key]) / 1e6 for key, end in consumes
                  if key in publications and end >= publications[key])
    if not ages:
        return {"count": 0}
    return {"count": len(ages), "p50": ages[math.ceil(.5 * len(ages)) - 1],
            "p95": ages[math.ceil(.95 * len(ages)) - 1], "max": ages[-1]}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--serial", required=True)
    parser.add_argument("--adb-root", action="store_true", help="use root adbd on a userdebug emulator")
    parser.add_argument("--cycles", type=int, default=8)
    parser.add_argument("--minutes", type=float, default=0,
                        help="run complete cycles for at least this duration, overriding --cycles")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--alternate-sizes", action="store_true",
                        help="alternate 720p and 1080p between cycles")
    parser.add_argument("--transport", choices=["flv", "annexb"], default="flv")
    args = parser.parse_args()
    if args.cycles < 1 or not math.isfinite(args.minutes) or args.minutes < 0:
        parser.error("--cycles must be positive and --minutes must be finite and nonnegative")
    args.output.mkdir(parents=True, exist_ok=False)
    class BenchmarkDevice(Device):
        def shell(self, cmd, root=True, timeout=30):
            return super().shell(cmd, root=root and not args.adb_root, timeout=timeout)

    d = BenchmarkDevice(args.serial)
    initial = d.status()
    if not initial or initial["feeder"]["running"] or initial["payload"].get("active"):
        raise RuntimeError("Stop the existing ReCam session before this test")
    camera_pid = initial["cameraserver"]["pid"]
    report = {"initial": initial, "cycles": [], "passed": False, "transport": args.transport,
              "requested_minutes": args.minutes}
    started_at = time.monotonic()
    feeder = encoder = logcat = None
    forwarded = None
    injected = False
    remote_file = f"/data/local/tmp/recam-stability-{time.time_ns()}.mp4"
    logpath = args.output / "device.log"

    def state():
        value = d.status()
        if not value or value["cameraserver"]["pid"] != camera_pid:
            raise RuntimeError(f"cameraserver disappeared or restarted: {value}")
        return value

    def wait_for(check, timeout=6):
        deadline = time.monotonic() + timeout
        while not check():
            if time.monotonic() >= deadline:
                raise RuntimeError("Timed out waiting for test precondition")
            time.sleep(.1)

    def log_since(offset):
        with logpath.open("rb") as log:
            log.seek(offset)
            return log.read().decode(errors="replace")

    def stop_encoder():
        nonlocal encoder
        if encoder and encoder.poll() is None:
            encoder.send_signal(signal.SIGCONT)
            encoder.terminate()
            try:
                encoder.wait(timeout=5)
            except subprocess.TimeoutExpired:
                encoder.kill()
                encoder.wait()
        encoder = None

    try:
        clip = args.output / "fallback.mp4"
        subprocess.run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-f", "lavfi",
                        "-i", "testsrc2=size=320x240:rate=15", "-t", "1",
                        "-c:v", "libx264", "-pix_fmt", "yuv420p", str(clip)], check=True)
        rc, _, err = d._run(["push", str(clip), remote_file])
        if rc:
            raise RuntimeError(err)
        report["build"] = d.ensure_deployed()
        rc, port, err = d._run(["forward", "tcp:0", f"tcp:{STREAM_PORT}"])
        if rc:
            raise RuntimeError(err)
        forwarded = int(port)
        with logpath.open("w") as log, (args.output / "processes.log").open("w") as proc_log:
            logcat = subprocess.Popen(d._argv([
                "logcat", "-v", "threadtime", "-T", "1", "-b", "main", "-b", "crash",
                "-s", "ReCam:I", "KittyMemoryEx:I", "ReCamLatency:I", "libc:F", "*:S"
            ]), stdout=log, stderr=proc_log)
            for cycle in itertools.count():
                if args.minutes:
                    if cycle and time.monotonic() - started_at >= args.minutes * 60:
                        break
                elif cycle >= args.cycles:
                    break
                offset = logpath.stat().st_size
                injected = True
                d.inject()
                wait_for(lambda: bool(state().get("ring", {}).get("lanes")))
                mode = ("disconnect", "stop_live", "pause_resume", "reconnect")[cycle % 4]
                source = remote_file if (cycle // 4) % 2 else remote_file + ".absent"
                command = f"RECAM_PROFILE=1 exec /data/local/tmp/recam feed {source}"
                if not args.adb_root:
                    command = f"su -c '{command}'"
                feeder = subprocess.Popen(d._argv(["shell", command]),
                                          stdout=proc_log, stderr=proc_log)
                wait_for(lambda: "feed: listening on" in log_since(offset))
                feeder_pid = state()["feeder"]["pid"]
                size = "1920x1080" if args.alternate_sizes and cycle % 2 else "1280x720"
                row = {"cycle": cycle + 1, "mode": mode, "size": size, "file_fallback": source == remote_file,
                       "feeder_pid": feeder_pid}
                report["cycles"].append(row)

                def start_encoder():
                    return subprocess.Popen(build_command("ffmpeg", [
                        "-re", "-f", "lavfi", "-i", f"testsrc2=size={size}:rate=30",
                        "-t", "3"
                    ], forwarded, transport=args.transport), stdout=proc_log, stderr=proc_log)

                encoder = start_encoder()
                wait_for(lambda: "event=publish" in log_since(offset))
                time.sleep(.5)
                if mode == "pause_resume":
                    encoder.send_signal(signal.SIGSTOP)
                    time.sleep(.4)  # drain the frames already sent
                    before = state()["ring"]["published"]
                    time.sleep(.4)
                    row["short_pause_publications"] = state()["ring"]["published"] - before
                    if row["short_pause_publications"]:
                        raise RuntimeError("New publications during the settled short pause")
                    time.sleep(2)
                    before = state()["ring"]["published"]
                    time.sleep(.5)
                    row["fallback_publications"] = state()["ring"]["published"] - before
                    if row["fallback_publications"] <= 0:
                        raise RuntimeError("Stalled stream did not restore fallback")
                    encoder.send_signal(signal.SIGCONT)
                if mode != "stop_live":
                    if encoder.wait(timeout=12):
                        raise RuntimeError("FFmpeg failed")
                    encoder = None
                    time.sleep(.3)
                    if mode == "reconnect":
                        encoder = start_encoder()
                        if encoder.wait(timeout=12):
                            raise RuntimeError("Reconnect encoder failed")
                        encoder = None
                d.shell(f"kill -TERM {feeder_pid}")
                row["feeder_exit"] = feeder.wait(timeout=8)
                feeder = None
                stop_encoder()
                if row["feeder_exit"]:
                    raise RuntimeError(f"Feeder exit failure: {row}")
                d.stop_payload()
                injected = False
                time.sleep(.4)
                stopped = state()
                if stopped["payload"].get("active") or stopped["feeder"]["running"]:
                    raise RuntimeError(f"Stop left an active component: {stopped}")
                evidence = log_since(offset)
                row["crashes"] = crash_lines(evidence, {camera_pid, feeder_pid})
                # Match a live publication with camera consumption of that sequence.
                pubs = set(re.findall(r"event=publish lane=(\d+) seq=(\d+)", evidence))
                consumed = set(re.findall(r"event=consume lane=(\d+) seq=(\d+)", evidence))
                row["live_frames_consumed"] = len(pubs & consumed)
                row["receive_to_copy_age_ms"] = frame_ages(evidence)
                if row["crashes"] or not row["live_frames_consumed"]:
                    raise RuntimeError(f"Crash or missing live camera consumption: {row}")
                if mode == "reconnect" and evidence.count("feed: a sender connected") != 2:
                    raise RuntimeError("Reconnect did not reach the feeder")
                rc, status_text, _ = d.shell(f"cat /proc/{camera_pid}/status")
                rss = re.search(r"^VmRSS:\s+(\d+)", status_text, re.MULTILINE)
                if rc == 0 and rss:
                    row["cameraserver_rss_kib"] = int(rss[1])
                row["elapsed_seconds"] = time.monotonic() - started_at
                (args.output / "progress.json").write_text(json.dumps(report, indent=2) + "\n")
                print(json.dumps(row), flush=True)
            report["passed"] = True
    except Exception as exc:
        report["error"] = str(exc)
        raise
    finally:
        cleanup_errors = []

        def cleanup(action):
            try:
                action()
            except Exception as exc:
                cleanup_errors.append(str(exc))

        cleanup(stop_encoder)
        if feeder:
            cleanup(d.stop_feeder)
            cleanup(lambda: feeder.wait(timeout=8))
            if feeder.poll() is None:
                cleanup(feeder.terminate)
        if injected:
            cleanup(d.stop_payload)
        if forwarded:
            cleanup(lambda: d.remove_forward(forwarded))
        cleanup(lambda: d.shell(f"rm -f {remote_file}"))
        if logcat:
            cleanup(logcat.terminate)
            cleanup(lambda: logcat.wait(timeout=5))
        cleanup(lambda: report.update(final=d.status()))
        if cleanup_errors:
            report.update(passed=False, cleanup_errors=cleanup_errors)
        report["elapsed_seconds"] = time.monotonic() - started_at
        (args.output / "report.json").write_text(json.dumps(report, indent=2) + "\n")
        if cleanup_errors:
            raise RuntimeError(f"Cleanup failures: {cleanup_errors}")


if __name__ == "__main__":
    main()
