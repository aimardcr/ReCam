"""ReCam desktop console."""

import os
import threading
import time

from PySide6.QtCore import Qt, QThread, QTimer, Signal, QObject
from PySide6.QtWidgets import (
    QApplication, QButtonGroup, QComboBox, QFileDialog, QGridLayout, QGroupBox,
    QHBoxLayout, QLabel, QLineEdit, QMainWindow, QPlainTextEdit, QPushButton,
    QRadioButton, QVBoxLayout, QWidget,
)

from .device import Device, STREAM_PORT
from .streamer import (Streamer, camera_input, ffmpeg_input,
                       list_cameras, rtmp_input, rtmp_publish_hint)

SRC_PATTERN, SRC_FILE, SRC_FFMPEG, SRC_RTMP, SRC_CAMERA = range(5)


class Job(QObject):
    """One blocking device operation on a worker thread."""
    done = Signal(bool, str)

    def __init__(self, fn):
        super().__init__()
        self._fn = fn

    def run(self):
        try:
            self.done.emit(True, self._fn() or "")
        except Exception as e:                      # adb and ffmpeg fail in many ways
            self.done.emit(False, str(e))


class Window(QMainWindow):
    _log_line = Signal(str)
    _cameras_found = Signal(list)

    def __init__(self):
        super().__init__()
        self.setWindowTitle("ReCam")
        self.resize(760, 620)

        self.device = None
        self.streamer = Streamer(on_line=self._log_line.emit)
        self._thread = None
        self._job = None
        self._label = ""
        self._prev = None                            # (now_ns, published) for the rate
        self._busy = False

        self._scanning = False
        self._log_line.connect(self._log)
        self._cameras_found.connect(self._on_cameras)
        self._build()
        self.refresh_devices()
        self.rescan_cameras()

        self._poll = QTimer(self)
        self._poll.timeout.connect(self.poll_status)
        self._poll.start(1000)

    # ---------------------------------------------------------------- layout

    def _build(self):
        root = QWidget()
        outer = QVBoxLayout(root)

        bar = QHBoxLayout()
        bar.addWidget(QLabel("Device"))
        self.devices = QComboBox()
        bar.addWidget(self.devices, 1)
        self.btn_refresh = QPushButton("Refresh")
        self.btn_refresh.clicked.connect(self.refresh_devices)
        bar.addWidget(self.btn_refresh)
        outer.addLayout(bar)

        box = QGroupBox("State")
        grid = QGridLayout(box)
        self.lbl_payload = QLabel("-")
        self.lbl_feeder = QLabel("-")
        self.lbl_lanes = QLabel("-")
        self.lbl_rate = QLabel("-")
        for row, (name, w) in enumerate([("Payload", self.lbl_payload),
                                         ("Feeder", self.lbl_feeder),
                                         ("Lanes", self.lbl_lanes),
                                         ("Rate", self.lbl_rate)]):
            grid.addWidget(QLabel(name), row, 0)
            w.setTextInteractionFlags(Qt.TextSelectableByMouse)
            grid.addWidget(w, row, 1)
        grid.setColumnStretch(1, 1)
        outer.addWidget(box)

        src = QGroupBox("Source")
        sv = QGridLayout(src)
        self.group = QButtonGroup(self)
        row = 0

        self.rb_pattern = QRadioButton("Test pattern")
        self.rb_pattern.setChecked(True)
        self.group.addButton(self.rb_pattern, SRC_PATTERN)
        sv.addWidget(self.rb_pattern, row, 0, 1, 3)
        row += 1

        self.rb_rtmp = QRadioButton("RTMP")
        self.group.addButton(self.rb_rtmp, SRC_RTMP)
        self.lbl_rtmp = QLabel()
        self.lbl_rtmp.setTextInteractionFlags(Qt.TextSelectableByMouse)
        server, key = rtmp_publish_hint()
        self.lbl_rtmp.setText(f"publish to Server: {server}, Stream Key: {key}")
        sv.addWidget(self.rb_rtmp, row, 0)
        sv.addWidget(self.lbl_rtmp, row, 1, 1, 2)
        row += 1

        self.rb_file = QRadioButton("Video file")
        self.group.addButton(self.rb_file, SRC_FILE)
        self.ed_file = QLineEdit()
        self.ed_file.setPlaceholderText("a local .mp4; pushed to the device and looped")
        b_file = QPushButton("Browse")
        b_file.clicked.connect(lambda: self._browse(self.ed_file))
        sv.addWidget(self.rb_file, row, 0)
        sv.addWidget(self.ed_file, row, 1)
        sv.addWidget(b_file, row, 2)
        row += 1

        self.rb_ffmpeg = QRadioButton("FFmpeg")
        self.group.addButton(self.rb_ffmpeg, SRC_FFMPEG)
        self.ed_ffmpeg = QLineEdit()
        self.ed_ffmpeg.setPlaceholderText(
            "a path, a URL, or raw input flags: -f lavfi -i testsrc2=size=1280x720:rate=30")
        sv.addWidget(self.rb_ffmpeg, row, 0)
        sv.addWidget(self.ed_ffmpeg, row, 1, 1, 2)
        row += 1

        self.rb_camera = QRadioButton("Camera")
        self.group.addButton(self.rb_camera, SRC_CAMERA)
        self.cb_camera = QComboBox()
        # Clicking the row rescans, so a device started after launch still appears.
        self.rb_camera.clicked.connect(self.rescan_cameras)
        sv.addWidget(self.rb_camera, row, 0)
        sv.addWidget(self.cb_camera, row, 1, 1, 2)

        sv.setColumnStretch(1, 1)
        outer.addWidget(src)

        run = QHBoxLayout()
        self.btn_start = QPushButton("Start")
        self.btn_start.clicked.connect(self.start)
        run.addWidget(self.btn_start)
        self.btn_stop = QPushButton("Stop")
        self.btn_stop.clicked.connect(self.stop)
        run.addWidget(self.btn_stop)
        run.addStretch()
        outer.addLayout(run)

        self.log = QPlainTextEdit()
        self.log.setReadOnly(True)
        self.log.setMaximumBlockCount(2000)
        outer.addWidget(self.log, 1)

        self.setCentralWidget(root)

    def rescan_cameras(self):
        """Enumerating spawns ffmpeg, so never do it on the GUI thread."""
        if self._scanning:
            return
        self._scanning = True
        threading.Thread(target=self._scan_cameras, daemon=True).start()

    def _scan_cameras(self):
        try:
            found = list_cameras()
        except Exception:
            found = []
        self._cameras_found.emit(found)

    def _on_cameras(self, found):
        self._scanning = False
        keep = self.cb_camera.currentText()
        self.cb_camera.clear()
        self.cb_camera.addItems(found)
        if keep in found:
            self.cb_camera.setCurrentText(keep)

    def _browse(self, target):
        path, _ = QFileDialog.getOpenFileName(
            self, "Choose a video", "", "Video (*.mp4 *.mkv *.mov *.webm);;All files (*)")
        if path:
            target.setText(path)

    def _log(self, msg):
        self.log.appendPlainText(f"{time.strftime('%H:%M:%S')}  {msg}")

    # ---------------------------------------------------------------- devices

    def refresh_devices(self):
        self.devices.clear()
        try:
            found = Device.list_devices()
        except Exception as e:
            self._log(f"adb: {e}")
            return
        self.devices.addItems(found)
        self._log(f"{len(found)} device(s)" if found else "no devices; is USB debugging on?")

    def _dev(self, quiet=False):
        serial = self.devices.currentText()
        if not serial:
            if not quiet:
                self._log("no device selected")
            return None
        if self.device is None or self.device.serial != serial:
            self.device = Device(serial)
        return self.device

    # ---------------------------------------------------------------- jobs

    def _start_job(self, fn, label):
        if self._busy:
            self._log("still busy")
            return
        self._busy = True
        self._set_enabled(False)
        self._log(f"{label}…")

        self._label = label
        self._thread = QThread(self)
        self._job = Job(fn)
        self._job.moveToThread(self._thread)
        self._thread.started.connect(self._job.run)
        # A bound method of this window, so Qt queues it back to the GUI thread; a bare
        # lambda would run on the worker and touch widgets from the wrong thread.
        self._job.done.connect(self._job_done)
        self._thread.start()

    def _job_done(self, ok, msg):
        self._thread.quit()
        self._thread.wait()
        self._thread.deleteLater()
        self._thread = None
        self._job = None

        self._busy = False
        self._set_enabled(True)
        for line in (msg or "").splitlines():
            self._log(line)
        self._log(f"{self._label}: {'ok' if ok else 'FAILED'}")

    def _set_enabled(self, on):
        for w in (self.btn_start, self.btn_stop, self.btn_refresh):
            w.setEnabled(on)

    # ---------------------------------------------------------------- actions

    def start(self):
        dev = self._dev()
        if not dev:
            return

        mode = self.group.checkedId()
        file_path = self.ed_file.text().strip()
        ff_text = self.ed_ffmpeg.text().strip()
        camera = self.cb_camera.currentText()

        if mode == SRC_FILE and not os.path.isfile(file_path):
            self._log("pick a video file first")
            return
        if mode == SRC_FFMPEG and not ff_text:
            self._log("give ffmpeg an input first")
            return
        if mode == SRC_CAMERA and not camera:
            self._log("no camera selected")
            return

        def work():
            out = [dev.ensure_deployed()]
            if mode == SRC_FILE:
                out.append("pushing source to the device…")
                dev.push_source(file_path)
            else:
                dev.clear_source()

            rc, _, se = dev.inject()
            out.append(f"inject rc={rc}" + (f" {se}" if se else ""))

            dev.stop_feeder()
            dev.start_feeder()
            out.append("feeder started")

            if mode in (SRC_FFMPEG, SRC_RTMP, SRC_CAMERA):
                dev.forward(STREAM_PORT)
                out.append(f"forwarded tcp:{STREAM_PORT}")
                if mode == SRC_FFMPEG:
                    args = ffmpeg_input(ff_text)
                elif mode == SRC_RTMP:
                    args = rtmp_input()
                    server, key = rtmp_publish_hint()
                    out.append(f"waiting for a publisher - Server {server}, Key {key}")
                else:
                    args = camera_input(camera)
                self.streamer.start(args, STREAM_PORT)
            return "\n".join(out)

        self._start_job(work, "Start")

    def stop(self):
        dev = self._dev()
        if not dev:
            return

        def work():
            self.streamer.stop()
            dev.stop_feeder()
            dev.stop_payload()
            dev.remove_forward(STREAM_PORT)
            return "streamer, feeder and payload stopped; forward removed"

        self._prev = None
        self._start_job(work, "Stop")

    # ---------------------------------------------------------------- polling

    def poll_status(self):
        if self._busy:
            return
        dev = self._dev(quiet=True)
        if dev is None:
            return

        st = dev.status()
        if st is None:
            self.lbl_payload.setText("no answer from recam_status")
            return

        if not st.get("root", False):
            self.lbl_payload.setText("cannot determine - not running as root")
            return

        p, ring = st.get("payload", {}), st.get("ring", {})

        if not p.get("injected"):
            self.lbl_payload.setText("not injected")
        elif p.get("control_version") != p.get("speaks_version"):
            self.lbl_payload.setText(
                f"version mismatch (device {p.get('control_version')}, "
                f"tools {p.get('speaks_version')}) - re-inject")
        elif p.get("active"):
            self.lbl_payload.setText(
                f"active, generation {p.get('generation')}, "
                f"{p.get('hooks_installed')} hook(s)")
        else:
            self.lbl_payload.setText(f"loaded but dormant, generation {p.get('generation')}")

        f = st.get("feeder", {})
        live = " + live stream" if self.streamer.running() else ""
        self.lbl_feeder.setText(f"running, pid {f.get('pid')}{live}" if f.get("running")
                                else "not running")

        lanes = ring.get("lanes", [])
        if not lanes:
            self.lbl_lanes.setText("none - the camera is not streaming")
        else:
            self.lbl_lanes.setText("   ".join(
                f"{l['width']}x{l['height']} "
                f"{'served' if l['served'] else 'waiting'} {l['age_ms']}ms"
                for l in lanes))

        self._update_rate(st, ring)

    def _update_rate(self, st, ring):
        now, pub = st.get("now_ns"), ring.get("published")
        if now is None or pub is None:
            self.lbl_rate.setText("-")
            return

        if self._prev:
            dt = (now - self._prev[0]) / 1e9
            dn = pub - self._prev[1]
            if dt > 0.2 and dn >= 0:
                self.lbl_rate.setText(
                    f"{dn / dt:.1f} fps published, "
                    f"{ring.get('dropped', 0)} dropped since injection")
        self._prev = (now, pub)

    def closeEvent(self, e):
        self.streamer.stop()
        if self.device:
            self.device.remove_forward(STREAM_PORT)
        super().closeEvent(e)


def main():
    app = QApplication([])
    w = Window()
    w.show()
    return app.exec()
