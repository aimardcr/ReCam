"""The desktop half of live feeding: ffmpeg into the forwarded TCP port."""

import os
import re
import shlex
import shutil
import subprocess
import threading

_NO_WINDOW = subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0

RTMP_PORT = 1935
RTMP_APP = "live"
RTMP_KEY = "recam"


class StreamerError(Exception):
    pass


# ---------------------------------------------------------------- input sides

def camera_input(name):
    if os.name == "nt":
        return ["-f", "dshow", "-i", f"video={name}"]
    return ["-f", "v4l2", "-i", name]


def rtmp_input(port=RTMP_PORT, app=RTMP_APP, key=RTMP_KEY):
    """ffmpeg's own demuxer listens, so there is no RTMP server to install."""
    return ["-listen", "1", "-f", "flv", "-i", f"rtmp://0.0.0.0:{port}/{app}/{key}"]


# The listener rejects any publisher whose stream key is not the one in its URL.
def rtmp_publish_hint(port=RTMP_PORT, app=RTMP_APP, key=RTMP_KEY):
    return f"rtmp://127.0.0.1:{port}/{app}", key


def ffmpeg_input(text):
    """Leading '-' means the user is supplying input flags; otherwise it is a path."""
    text = text.strip()
    if text.startswith("-"):
        return shlex.split(text)
    args = ["-re"]
    if os.path.isfile(text):
        args += ["-stream_loop", "-1"]
    return args + ["-i", text]


def file_input(path, loop=True):
    args = ["-re"]
    if loop:
        args += ["-stream_loop", "-1"]
    return args + ["-i", path]


# ---------------------------------------------------------------- enumeration

_DSHOW_LINE = re.compile(r'"([^"]+)"\s+\((\w+)\)')


def list_cameras(ffmpeg=None):
    ffmpeg = ffmpeg or shutil.which("ffmpeg")
    if not ffmpeg:
        return []

    if os.name != "nt":
        return sorted(f"/dev/{d}" for d in os.listdir("/dev")
                      if d.startswith("video")) if os.path.isdir("/dev") else []

    p = subprocess.run([ffmpeg, "-hide_banner", "-list_devices", "true",
                        "-f", "dshow", "-i", "dummy"],
                       capture_output=True, text=True, timeout=30,
                       creationflags=_NO_WINDOW)
    out = []
    for line in p.stderr.splitlines():
        m = _DSHOW_LINE.search(line)
        # "(none)" devices are registered filters with no pin we can capture from.
        if m and m.group(2) == "video" and m.group(1) not in out:
            out.append(m.group(1))
    return out


# ---------------------------------------------------------------- the child

def build_command(ffmpeg, input_args, port, fps=30, max_height=1080):
    """`-bf 0` is not optional: the device synthesises timestamps in decode order."""
    return [ffmpeg, "-hide_banner", "-loglevel", "warning"] + list(input_args) + [
        "-an",
        "-c:v", "libx264",
        "-preset", "ultrafast",
        "-tune", "zerolatency",
        "-bf", "0",
        "-g", "15",
        "-r", str(fps),
        "-vf", f"scale=-2:'min({max_height},ih)'",
        "-pix_fmt", "yuv420p",
        "-f", "h264", f"tcp://127.0.0.1:{port}",
    ]


class Streamer:
    """One ffmpeg child. Its stderr is pumped to `on_line` so the UI can show it."""

    def __init__(self, on_line=None, ffmpeg=None):
        self.ffmpeg = ffmpeg or shutil.which("ffmpeg")
        self._on_line = on_line or (lambda s: None)
        self._proc = None

    def running(self):
        return self._proc is not None and self._proc.poll() is None

    def start(self, input_args, port, **kw):
        if not self.ffmpeg:
            raise StreamerError("ffmpeg is not on PATH")
        if self.running():
            raise StreamerError("already streaming")

        argv = build_command(self.ffmpeg, input_args, port, **kw)
        self._on_line(" ".join(argv))
        self._proc = subprocess.Popen(argv, stdout=subprocess.DEVNULL,
                                      stderr=subprocess.PIPE, text=True,
                                      bufsize=1, creationflags=_NO_WINDOW)
        threading.Thread(target=self._drain, args=(self._proc,), daemon=True).start()

    def _drain(self, proc):
        for line in proc.stderr:
            line = line.strip()
            if line:
                self._on_line(f"ffmpeg: {line}")
        rc = proc.wait()
        if rc not in (0, 1, -15, 255):
            self._on_line(f"ffmpeg exited {rc}")

    def stop(self):
        if not self.running():
            self._proc = None
            return
        self._proc.terminate()
        try:
            self._proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self._proc.kill()
        self._proc = None
