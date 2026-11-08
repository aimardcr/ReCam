#!/usr/bin/env python3
"""Exercise Qt construction and startup of the frozen desktop on its native OS."""
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
os.environ["QT_QPA_PLATFORM"] = "offscreen"
sys.path.insert(0, str(ROOT / "desktop"))


def main():
    from PySide6.QtWidgets import QApplication
    from recam_ui.app import Window
    app = QApplication([])
    window = Window()
    window.show()
    app.processEvents()
    assert not window.grab().isNull(), "Qt window did not render"
    window.close()
    app.processEvents()
    if sys.platform == "win32":
        executable = ROOT / "dist/ReCam/ReCam.exe"
    elif sys.platform == "darwin":
        executable = ROOT / "dist/ReCam.app/Contents/MacOS/ReCam"
    else:
        executable = ROOT / "dist/ReCam/ReCam"
    # Remove Python/Qt environment paths installed by importing PySide6 above;
    # the frozen application must resolve its own bundled libraries/plugins.
    env = dict(os.environ)
    for name in ("PYTHONHOME", "PYTHONPATH", "QT_PLUGIN_PATH", "QT_QPA_PLATFORM_PLUGIN_PATH"):
        env.pop(name, None)
    with (ROOT / "build/frozen-startup.log").open("w+") as log:
        process = subprocess.Popen([str(executable)], cwd=executable.parent,
                                   env=env, stdout=log, stderr=log)
        try:
            try:
                code = process.wait(timeout=8)
            except subprocess.TimeoutExpired:
                print("Frozen desktop remained running during the startup check")
            else:
                log.seek(0)
                raise RuntimeError(f"Frozen desktop exited early ({code}): {log.read()}")
        finally:
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill(); process.wait()
        log.seek(0)
        output = log.read()
        print(output)
        if "Traceback (most recent call last)" in output:
            raise RuntimeError("Frozen startup logged a Python exception")


if __name__ == "__main__":
    main()
