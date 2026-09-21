#!/usr/bin/env python3
"""Build the device binaries, fold them into the UI, and freeze a release.

    python tools/package.py              # build + bundle + freeze
    python tools/package.py --no-ndk     # reuse libs/arm64-v8a as it stands
    python tools/package.py --no-freeze  # stage the payload only
"""

import argparse
import os
import shutil
import subprocess
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ANDROID = os.path.join(REPO, "android")
LIBS = os.path.join(ANDROID, "libs", "arm64-v8a")
PAYLOAD = os.path.join(REPO, "desktop", "recam_ui", "payload", "arm64-v8a")
DIST = os.path.join(REPO, "dist")

sys.path.insert(0, os.path.join(REPO, "desktop"))
from recam_ui.device import BINARIES, build_id      # noqa: E402


def run(argv, cwd=None):
    print("+", " ".join(argv))
    if subprocess.call(argv, cwd=cwd) != 0:
        sys.exit(f"failed: {argv[0]}")


def ndk_build():
    ndk = os.environ.get("ANDROID_NDK_HOME") or os.environ.get("NDK_HOME")
    exe = "ndk-build.cmd" if os.name == "nt" else "ndk-build"
    cmd = os.path.join(ndk, exe) if ndk else shutil.which(exe)
    if not cmd:
        sys.exit("ndk-build not found; set ANDROID_NDK_HOME or pass --no-ndk")
    run([cmd, "NDK_PROJECT_PATH=.", "NDK_APPLICATION_MK=jni/Application.mk",
         "APP_ABI=arm64-v8a", "-j8"], cwd=ANDROID)


def stage():
    missing = [b for b in BINARIES if not os.path.isfile(os.path.join(LIBS, b))]
    if missing:
        sys.exit(f"not built: {', '.join(missing)}")

    shutil.rmtree(PAYLOAD, ignore_errors=True)
    os.makedirs(PAYLOAD)
    for b in BINARIES:
        shutil.copy2(os.path.join(LIBS, b), os.path.join(PAYLOAD, b))
    bid = build_id(PAYLOAD)
    print(f"staged {len(BINARIES)} binaries -> {PAYLOAD}  (build {bid})")
    return bid


def freeze():
    try:
        import PyInstaller                           # noqa: F401
    except ImportError:
        sys.exit("pip install pyinstaller, or pass --no-freeze")

    sep = ";" if os.name == "nt" else ":"
    run([sys.executable, "-m", "PyInstaller", "--noconfirm", "--clean",
         "--name", "ReCam", "--windowed", "--onedir",
         "--distpath", DIST, "--workpath", os.path.join(REPO, "build", "pyi"),
         "--specpath", os.path.join(REPO, "build"),
         "--add-data", f"{PAYLOAD}{sep}payload/arm64-v8a",
         os.path.join(REPO, "desktop", "main.py")])
    print(f"\nrelease tree: {os.path.join(DIST, 'ReCam')}")
    print("adb and ffmpeg are NOT bundled; both must be on PATH.")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--no-ndk", action="store_true", help="skip ndk-build")
    ap.add_argument("--no-freeze", action="store_true", help="stage the payload only")
    a = ap.parse_args()

    if not a.no_ndk:
        ndk_build()
    stage()
    if not a.no_freeze:
        freeze()


if __name__ == "__main__":
    main()
