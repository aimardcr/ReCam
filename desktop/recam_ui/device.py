"""Every adb invocation lives here; nothing else builds an adb command line."""

import hashlib
import json
import os
import shutil
import subprocess
import sys

REMOTE_DIR = "/data/local/tmp"
REMOTE_MARKER = f"{REMOTE_DIR}/.recam_build"
REMOTE_SOURCE = f"{REMOTE_DIR}/recam_source.mp4"
STREAM_PORT = 27183

BINARIES = ["librecam.so", "recam_inject", "recam_stop", "recam_status",
            "recam_feed", "recam_watch"]
EXECUTABLES = [b for b in BINARIES if not b.endswith(".so")]

# Windows would otherwise flash a console window for every adb call.
_NO_WINDOW = subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0


class DeviceError(Exception):
    pass


def payload_dir():
    """Frozen bundle, then a packaged copy beside this module, then the build tree."""
    candidates = []
    if getattr(sys, "_MEIPASS", None):
        candidates.append(os.path.join(sys._MEIPASS, "payload", "arm64-v8a"))
    here = os.path.dirname(os.path.abspath(__file__))
    candidates.append(os.path.join(here, "payload", "arm64-v8a"))
    repo = os.path.dirname(os.path.dirname(here))
    candidates.append(os.path.join(repo, "android", "libs", "arm64-v8a"))

    for c in candidates:
        if all(os.path.isfile(os.path.join(c, b)) for b in BINARIES):
            return c
    return None


def build_id(libs_dir):
    """Content hash, so a rebuild is re-pushed and an unchanged one is not."""
    h = hashlib.sha256()
    for b in BINARIES:
        h.update(b.encode())
        with open(os.path.join(libs_dir, b), "rb") as f:
            h.update(f.read())
    return h.hexdigest()[:16]


class Device:
    def __init__(self, serial=None, adb=None):
        self.serial = serial
        self.adb = adb or shutil.which("adb")
        if not self.adb:
            raise DeviceError("adb is not on PATH")

    # ---------------------------------------------------------------- plumbing

    def _argv(self, args):
        base = [self.adb]
        if self.serial:
            base += ["-s", self.serial]
        return base + list(args)

    def _run(self, args, timeout=30):
        p = subprocess.run(self._argv(args), capture_output=True, text=True,
                           timeout=timeout, creationflags=_NO_WINDOW)
        return p.returncode, p.stdout.strip(), p.stderr.strip()

    def shell(self, cmd, root=True, timeout=30):
        """Root by default: every ReCam tool needs it."""
        return self._run(["shell", f"su -c '{cmd}'" if root else cmd], timeout)

    @staticmethod
    def list_devices(adb=None):
        adb = adb or shutil.which("adb")
        if not adb:
            return []
        p = subprocess.run([adb, "devices"], capture_output=True, text=True,
                           timeout=15, creationflags=_NO_WINDOW)
        out = []
        for line in p.stdout.splitlines()[1:]:
            parts = line.split()
            if len(parts) == 2 and parts[1] == "device":
                out.append(parts[0])
        return out

    # ---------------------------------------------------------------- lifecycle

    def deploy(self, libs_dir):
        for b in BINARIES:
            rc, _, err = self._run(["push", os.path.join(libs_dir, b),
                                    f"{REMOTE_DIR}/{b}"], timeout=120)
            if rc != 0:
                raise DeviceError(f"push {b}: {err}")

        chmod = " ".join(f"{REMOTE_DIR}/{b}" for b in EXECUTABLES)
        self.shell(f"chmod 755 {chmod}")
        self.shell(f"echo {build_id(libs_dir)} > {REMOTE_MARKER}")

    def ensure_deployed(self):
        """Push only when the device is missing this exact build. Returns a log line."""
        libs = payload_dir()
        if not libs:
            raise DeviceError("no device binaries found; build them or run a packaged "
                              "release")

        want = build_id(libs)
        _, have, _ = self.shell(f"cat {REMOTE_MARKER} 2>/dev/null")
        if have.strip() == want:
            return f"binaries already current ({want})"

        self.deploy(libs)
        return f"deployed build {want} from {libs}"

    def inject(self):
        return self.shell(f"{REMOTE_DIR}/recam_inject", timeout=120)

    def stop_payload(self):
        return self.shell(f"{REMOTE_DIR}/recam_stop", timeout=120)

    def push_source(self, local_path):
        rc, _, err = self._run(["push", local_path, REMOTE_SOURCE], timeout=600)
        if rc != 0:
            raise DeviceError(f"push source: {err}")

    def clear_source(self):
        self.shell(f"rm -f {REMOTE_SOURCE}")

    def start_feeder(self):
        # nohup + background, so the adb shell can return while it keeps running.
        self.shell(f"nohup {REMOTE_DIR}/recam_feed >/dev/null 2>&1 &")

    def stop_feeder(self):
        self.shell("pkill -f recam_feed")

    def status(self):
        rc, out, _ = self.shell(f"{REMOTE_DIR}/recam_status", timeout=15)
        if rc != 0 or not out:
            return None
        try:
            return json.loads(out)
        except json.JSONDecodeError:
            return None

    # ---------------------------------------------------------------- forwarding

    def forward(self, port=STREAM_PORT):
        rc, _, err = self._run(["forward", f"tcp:{port}", f"tcp:{port}"])
        if rc != 0:
            raise DeviceError(f"adb forward: {err}")

    def remove_forward(self, port=STREAM_PORT):
        self._run(["forward", "--remove", f"tcp:{port}"])
