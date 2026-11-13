"""Every adb invocation lives here; nothing else builds an adb command line."""

import hashlib
import json
import logging
import os
import re
import shutil
import subprocess
import sys
import threading
import time

REMOTE_DIR = "/data/local/tmp"
REMOTE_MARKER = f"{REMOTE_DIR}/.recam_build"
REMOTE_SOURCE = f"{REMOTE_DIR}/recam_source.mp4"
REMOTE_FEED_LOG = f"{REMOTE_DIR}/recam-feed-start.log"
STREAM_PORT = 27183

BINARIES = ["librecam.so", "recam"]
EXECUTABLES = [b for b in BINARIES if not b.endswith(".so")]

# Windows would otherwise flash a console window for every adb call.
_NO_WINDOW = subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0
_log = logging.getLogger("recam.adb")


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
        argv = self._argv(args)
        _log.info("Command: %r", argv)
        try:
            p = subprocess.run(argv, capture_output=True, text=True,
                               encoding="utf-8", errors="replace",
                               timeout=timeout, creationflags=_NO_WINDOW)
        except Exception:
            _log.exception("ADB command failed: %r", argv)
            raise
        _log.info("Exit %s; stdout=%s; stderr=%s", p.returncode, p.stdout.strip(), p.stderr.strip())
        return p.returncode, p.stdout.strip(), p.stderr.strip()

    def shell(self, cmd, root=True, timeout=30):
        """Root by default: every ReCam tool needs it."""
        return self._run(["shell", f"su -c '{cmd}'" if root else cmd], timeout)

    @staticmethod
    def list_devices(adb=None):
        adb = adb or shutil.which("adb")
        if not adb:
            return []
        _log.info("Command: %r", [adb, "devices"])
        p = subprocess.run([adb, "devices"], capture_output=True, text=True,
                           encoding="utf-8", errors="replace",
                           timeout=15, creationflags=_NO_WINDOW)
        _log.info("Exit %s; stdout=%s; stderr=%s", p.returncode, p.stdout.strip(), p.stderr.strip())
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

    def _payload_command(self, command):
        result = self.shell(f"{REMOTE_DIR}/recam {command}", timeout=120)
        rc, out, err = result
        if rc != 0:
            detail = "\n".join(s for s in (out, err) if s)
            if not detail:
                detail = "See adb logcat -s ReCam for device details."
            raise DeviceError(f"recam {command} failed (exit {rc}): {detail}")
        return result

    def inject(self):
        return self._payload_command("inject")

    def stop_payload(self):
        return self._payload_command("stop")

    def push_source(self, local_path):
        rc, _, err = self._run(["push", local_path, REMOTE_SOURCE], timeout=600)
        if rc != 0:
            raise DeviceError(f"push source: {err}")

    def clear_source(self):
        self.shell(f"rm -f {REMOTE_SOURCE}")

    def start_feeder(self):
        # Detach all three descriptors: nohup alone can retain adb's stdin.
        detail = ""
        pid = 0
        try:
            rc, out, err = self.shell(
                f"(umask 077; exec nohup {REMOTE_DIR}/recam feed "
                f"</dev/null >{REMOTE_FEED_LOG} 2>&1) & echo $!")
            pid = int(out.strip()) if out.strip().isdigit() else 0
            if rc or pid <= 0:
                raise DeviceError(f"Could not launch feeder: {out} {err}")
            deadline = time.monotonic() + 10
            while time.monotonic() < deadline:
                _, detail, _ = self.shell(f"tail -c 65536 {REMOTE_FEED_LOG}", timeout=5)
                st = self.status()
                if st is not None:
                    feeder = st.get("feeder", {})
                    payload = st.get("payload", {})
                    markers = re.findall(r"RECAM_FEED_READY pid=(\d+) generation=(\d+)", detail)
                    ready = markers[-1] if markers else None
                    if (feeder.get("running") and feeder.get("pid") == pid and ready
                            and int(ready[0]) == pid and payload.get("active")
                            and int(ready[1]) == payload.get("generation")):
                        _log.info("Feeder ready: pid=%s generation=%s", pid, ready[1])
                        return pid
                time.sleep(0.2)
            raise DeviceError(f"Feeder did not become ready (pid {pid}). "
                              f"{detail.strip() or 'See the saved device/payload logs.'}")
        except Exception:
            _log.exception("Feeder startup failed: pid=%s output=%s", pid, detail)
            try:
                self.stop_feeder()
            except Exception:
                _log.exception("Could not clean up failed feeder startup")
            raise

    def stop_feeder(self):
        # Anchor argv[0] and argv[1], excluding the shell and other subcommands.
        rc, out, err = self.shell(f'pkill -f "^{REMOTE_DIR}/recam feed( |$)"')
        if rc not in (0, 1):
            raise DeviceError(f"Could not stop feeder: {out} {err}")
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            st = self.status()
            if st is not None and not st.get("feeder", {}).get("running"):
                return
            time.sleep(0.2)
        raise DeviceError("Previous feeder did not stop; refusing to start a competing producer")

    def status(self):
        rc, out, _ = self.shell(f"{REMOTE_DIR}/recam status", timeout=15)
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


class DeviceLogs:
    """One reconnecting logcat reader, kept alive across Start/Stop on one device."""

    def __init__(self):
        self._stop = threading.Event()
        self._lock = threading.Lock()
        self._proc = None
        self._thread = None
        self._key = None
        self._ready = threading.Event()

    def start(self, device):
        key = (device.adb, device.serial)
        if self._key == key and self._thread and self._thread.is_alive():
            return
        self.stop()
        self._key = key
        self._stop.clear()
        self._ready.clear()
        argv = device._argv(["logcat", "-b", "all", "-v", "threadtime", "*:V"])
        self._thread = threading.Thread(target=self._capture, args=(argv, key), daemon=True)
        self._thread.start()
        if not self._ready.wait(timeout=5):
            _log.warning("Logcat reader has not started within five seconds")

    def _capture(self, argv, key):
        device_log = logging.getLogger("recam.device")
        payload_log = logging.getLogger("recam.payload")

        def record(raw):
            line = raw.decode("utf-8", errors="replace").rstrip("\r\n")
            device_log.info("[%s] %s", key[1], line)
            # Preserve camera/feeder messages separately from noisy system output.
            if any(tag in line for tag in ("ReCam", "recam", "KittyMemoryEx", "UBWC", "C2D", "qdMetaData")):
                payload_log.info("[%s] %s", key[1], line)

        while not self._stop.is_set():
            proc = None
            try:
                with self._lock:
                    if self._stop.is_set():
                        break
                    proc = subprocess.Popen(argv, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                            creationflags=_NO_WINDOW)
                    self._proc = proc
                self._ready.set()
                _log.info("Logcat connected: %s", key[1])
                device_log.info("Logcat capture/reconnect: %s (includes retained device history)", key[1])
                pending = b""
                while True:
                    chunk = proc.stdout.read1(65536)
                    if not chunk:
                        break
                    pending += chunk
                    while b"\n" in pending:
                        line, pending = pending.split(b"\n", 1)
                        record(line)
                    if len(pending) >= 65536:
                        record(pending)
                        pending = b""
                if pending:
                    record(pending)
                _log.info("Logcat exited: device=%s rc=%s", key[1], proc.wait())
            except Exception:
                self._ready.set()
                _log.exception("Logcat capture failed: %s", key[1])
            finally:
                if proc:
                    self._terminate(proc)
                    proc.stdout.close()
                with self._lock:
                    if self._proc is proc:
                        self._proc = None
            if self._stop.wait(2):
                break

    @staticmethod
    def _terminate(proc):
        if proc.poll() is None:
            try:
                proc.terminate()
                proc.wait(timeout=2)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait(timeout=2)
            except ProcessLookupError:
                pass

    def stop(self):
        self._stop.set()
        with self._lock:
            proc = self._proc
        if proc:
            self._terminate(proc)
        if self._thread:
            self._thread.join(timeout=5)
            if self._thread.is_alive():
                raise DeviceError("Logcat reader did not stop")
        self._thread = None
        self._key = None
