"""Persistent, bounded desktop and Android logs; no device-side files required."""

import logging
from logging.handlers import RotatingFileHandler
import os
from pathlib import Path
import platform
import re
import sys
import threading
import time


def _process_exists(pid):
    if os.name == "nt":
        import ctypes
        from ctypes import wintypes
        kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel.OpenProcess.argtypes = (wintypes.DWORD, wintypes.BOOL, wintypes.DWORD)
        kernel.OpenProcess.restype = wintypes.HANDLE
        kernel.CloseHandle.argtypes = (wintypes.HANDLE,)
        handle = kernel.OpenProcess(0x1000, False, pid)  # QUERY_LIMITED_INFORMATION
        if handle:
            kernel.CloseHandle(handle)
            return True
        return ctypes.get_last_error() != 87  # INVALID_PARAMETER: no such PID
    try:
        os.kill(pid, 0)
        return True
    except ProcessLookupError:
        return False
    except OSError:
        return True


def setup_logs():
    if getattr(sys, "frozen", False):
        base = Path(sys.executable).resolve().parent
        # Keep logs outside a macOS application bundle.
        for parent in base.parents:
            if parent.suffix == ".app":
                base = parent.parent
                break
    else:
        base = Path(__file__).resolve().parents[2]

    failure = None
    for directory in dict.fromkeys((base / "logs", Path.home() / "ReCam" / "logs")):
        handlers = []
        try:
            directory.mkdir(parents=True, exist_ok=True)
            # A separate set per process avoids concurrent rotation by two GUIs.
            for name, size, backups in (("desktop", 8, 3), ("device", 32, 3),
                                        ("payload", 8, 3)):
                handler = RotatingFileHandler(
                    directory / f"{name}-{os.getpid()}.log", maxBytes=size * 1024 * 1024,
                    backupCount=backups, encoding="utf-8", errors="backslashreplace")
                handler.setFormatter(logging.Formatter("%(asctime)s %(levelname)s %(message)s"))
                handlers.append((name, handler))
        except OSError as exc:
            failure = str(exc)
            for _, handler in handlers:
                handler.close()
            continue

        root = logging.getLogger("recam")
        root.setLevel(logging.INFO)
        root.propagate = False
        root.addHandler(handlers[0][1])
        for name, handler in handlers[1:]:
            logger = logging.getLogger(f"recam.{name}")
            logger.setLevel(logging.INFO)
            logger.propagate = False
            logger.addHandler(handler)
        root.info("Session started: pid=%s platform=%s Python=%s executable=%s",
                  os.getpid(), platform.platform(), sys.version, sys.executable)
        for candidate in (base / "BUILD-INFO.json", base / "../BUILD-INFO.json"):
            if candidate.is_file():
                try:
                    root.info("Build: %s", candidate.read_text(encoding="utf-8"))
                except OSError:
                    root.exception("Cannot read build metadata")
                break
        if failure:
            root.warning("Using fallback log directory: %s", failure)
        # Only our stale files; never delete arbitrary files from this directory.
        cutoff = time.time() - 7 * 24 * 60 * 60
        alive = {}
        for old in directory.iterdir():
            match = re.fullmatch(r"(?:desktop|device|payload)-(\d+)\.log(?:\.[1-3])?", old.name)
            if match:
                try:
                    if not old.is_symlink() and old.stat().st_mtime < cutoff:
                        pid = int(match[1])
                        if pid not in alive:
                            alive[pid] = _process_exists(pid)
                        if not alive[pid]:
                            old.unlink()
                except OSError:
                    root.warning("Could not remove expired log: %s", old)
        return directory
    raise OSError(f"Cannot create log files: {failure}")


def install_exception_logging():
    original = sys.excepthook

    def report(kind, value, traceback):
        logging.getLogger("recam").critical("Unhandled exception",
                                            exc_info=(kind, value, traceback))
        original(kind, value, traceback)

    sys.excepthook = report
    thread_original = threading.excepthook

    def thread_report(args):
        logging.getLogger("recam").critical(
            "Unhandled thread exception", exc_info=(args.exc_type, args.exc_value, args.exc_traceback))
        thread_original(args)

    threading.excepthook = thread_report
