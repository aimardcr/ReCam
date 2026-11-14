#!/usr/bin/env python3
"""Assemble versioned release archives from native builds; never publishes."""
import argparse
import hashlib
import importlib.metadata
import io
import json
from pathlib import Path
import platform
import re
import struct
import subprocess
import sys
import tarfile
import zipfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "desktop"))
from recam_ui.device import BINARIES, build_id

TARGETS = ("windows-x64", "windows-arm64", "macos-arm64", "macos-x64", "linux-x64")


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def metadata(version, target, payload):
    packages = {}
    for name in ("PySide6", "PyInstaller"):
        try:
            packages[name] = importlib.metadata.version(name)
        except importlib.metadata.PackageNotFoundError:
            pass
    return json.dumps({
        "version": version, "target": target,
        "commit": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip(),
        "python": platform.python_version(), "build_platform": platform.platform(),
        "packages": packages, "android_payload_build": build_id(str(payload)),
        "android_payload_sha256": {name: digest(payload / name) for name in BINARIES},
    }, indent=2) + "\n"


def check_elf(path, machine):
    header = path.read_bytes()[:20]
    if header[:6] != b"\x7fELF\x02\x01" or struct.unpack_from("<H", header, 18)[0] != machine:
        raise RuntimeError(f"Wrong ELF architecture: {path}")


def android(version, output):
    for abi, target, machine in (("arm64-v8a", "arm64", 183), ("x86_64", "x86_64", 62)):
        payload = ROOT / "android" / "libs" / abi
        for name in BINARIES:
            check_elf(payload / name, machine)
        usage = f"""ReCam device binaries (Android {abi}, API 30+). Root required.

  adb push recam librecam.so /data/local/tmp/
  adb shell su -c 'chmod 755 /data/local/tmp/recam'

Run on the device as root:
  recam inject   load the payload and install the hook
  recam feed     feed a file, live stream, or test pattern
  recam stop     remove the hook and restore the real camera
  recam status   print state as JSON
  recam watch    re-inject when cameraserver respawns

Update recam and librecam.so together, then stop/re-inject the payload.
The feeder accepts FLV/AVC or raw Annex-B H.264 on 127.0.0.1:27183 and plays
/data/local/tmp/recam_source.mp4 when no live source is available.
"""
        path = output / f"recam-cli-{version}-android-{target}.zip"
        with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
            for name in BINARIES:
                archive.write(payload / name, name)
            for name in ("LICENSE", "NOTICE"):
                archive.write(ROOT / name, name)
            archive.write(ROOT / "android/sepolicy/recam.rule", "recam.rule")
            archive.writestr("USAGE.txt", usage)
            archive.writestr("BUILD-INFO.json", metadata(version, f"android-{target}", payload))
        print(path)


def desktop(version, target, output):
    system, arch = target.split("-")
    expected_system = {"windows": "Windows", "macos": "Darwin", "linux": "Linux"}[system]
    actual_arch = platform.machine().lower()
    if platform.system() != expected_system or actual_arch not in (
            ("arm64", "aarch64") if arch == "arm64" else ("amd64", "x86_64")):
        raise RuntimeError(f"Build {target} on its native runner, got {platform.platform()}")
    bundle = ROOT / "dist" / ("ReCam.app" if system == "macos" else "ReCam")
    executable = bundle / ({"windows": "ReCam.exe", "macos": "Contents/MacOS/ReCam", "linux": "ReCam"}[system])
    binary = executable.read_bytes()
    if system == "windows":
        offset = struct.unpack_from("<I", binary, 0x3c)[0]
        expected = 0xaa64 if arch == "arm64" else 0x8664
        if binary[offset:offset + 4] != b"PE\0\0" or struct.unpack_from("<H", binary, offset + 4)[0] != expected:
            raise RuntimeError("Incorrect Windows executable architecture")
    elif system == "macos":
        expected = 0x100000c if arch == "arm64" else 0x1000007
        if binary[:4] != b"\xcf\xfa\xed\xfe" or struct.unpack_from("<I", binary, 4)[0] != expected:
            raise RuntimeError("Incorrect macOS executable architecture")
    else:
        check_elf(executable, 62)
    payload = ROOT / "android/libs/arm64-v8a"
    for name in BINARIES:
        matches = [p for p in bundle.rglob(name) if "payload" in p.parts and p.is_file()]
        if not matches or any(digest(p) != digest(payload / name) for p in matches):
            raise RuntimeError(f"Missing or stale bundled Android binary: {name}")
    info = metadata(version, target, payload)
    if system == "linux":
        path = output / f"ReCam-{version}-{target}.tar.gz"
        with tarfile.open(path, "w:gz") as archive:
            archive.add(bundle, arcname="ReCam", filter=lambda entry:
                        None if entry.name == "ReCam/logs" or entry.name.startswith("ReCam/logs/")
                        else entry)
            for name in ("LICENSE", "NOTICE", "README.md", "assets/recam-preview.jpg"):
                archive.add(ROOT / name, arcname=f"ReCam/{name}")
            entry = tarfile.TarInfo("ReCam/BUILD-INFO.json")
            data = info.encode(); entry.size = len(data); entry.mode = 0o644
            archive.addfile(entry, io.BytesIO(data))
    else:
        path = output / f"ReCam-{version}-{target}.zip"
        if system == "macos":
            subprocess.run(["ditto", "-c", "-k", "--sequesterRsrc", "--keepParent", str(bundle), str(path)], check=True)
        with zipfile.ZipFile(path, "a" if system == "macos" else "w", zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
            if system == "windows":
                for file in sorted(bundle.rglob("*")):
                    if file.is_file() and file.relative_to(bundle).parts[0] != "logs":
                        archive.write(file, file.relative_to(bundle).as_posix())
            for name in ("LICENSE", "NOTICE", "README.md", "assets/recam-preview.jpg"):
                archive.write(ROOT / name, name)
            archive.writestr("BUILD-INFO.json", info)
    print(path)


def checksums(version, output):
    expected = {f"ReCam-{version}-{target}.{'tar.gz' if target.startswith('linux') else 'zip'}" for target in TARGETS}
    expected |= {f"recam-cli-{version}-android-{abi}.zip" for abi in ("arm64", "x86_64")}
    actual = {p.name for p in output.iterdir() if p.name.endswith((".zip", ".tar.gz"))}
    if actual != expected:
        raise RuntimeError(f"Release assets differ: missing={expected - actual}, extra={actual - expected}")
    (output / "SHA256SUMS.txt").write_text("".join(f"{digest(output / name)}  {name}\n" for name in sorted(expected)))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("kind", choices=("android", "desktop", "checksums"))
    parser.add_argument("--version", required=True)
    parser.add_argument("--target", choices=TARGETS)
    parser.add_argument("--output", type=Path, default=ROOT / "release")
    args = parser.parse_args()
    if not re.fullmatch(r"v\d+\.\d+\.\d+", args.version):
        parser.error("version must be vMAJOR.MINOR.PATCH")
    args.output.mkdir(parents=True, exist_ok=True)
    if args.kind == "android": android(args.version, args.output)
    elif args.kind == "checksums": checksums(args.version, args.output)
    elif args.target: desktop(args.version, args.target, args.output)
    else: parser.error("desktop requires --target")


if __name__ == "__main__":
    main()
