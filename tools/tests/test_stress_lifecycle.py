import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from stress_lifecycle import crash_lines, frame_ages


class CrashDetection(unittest.TestCase):
    def test_fatal_background_thread_even_when_process_exits_zero(self):
        log = """11-08 18:37:05.569 4659 4659 I KittyMemoryEx: feed: stopping
11-08 18:37:05.669 4659 4673 F libc : FORTIFY: pthread_mutex_lock called on a destroyed mutex
11-08 18:37:05.670 4659 4673 F libc : Fatal signal 6 (SIGABRT)
"""
        self.assertEqual(len(crash_lines(log, {4659, 23424})), 2)

    def test_excludes_other_processes_and_normal_logs(self):
        log = """11-08 18:37:05.669 100 101 F libc : Fatal signal 6 (SIGABRT)
11-08 18:37:05.670 4659 4673 I KittyMemoryEx: decoder: stream output
"""
        self.assertEqual(crash_lines(log, {4659, 23424}), [])


class FrameAge(unittest.TestCase):
    def test_matches_lane_and_sequence_and_keeps_stale_reuse(self):
        log = """event=publish lane=0 seq=2 receive=1000000
 event=consume lane=1 seq=2 end=900000000
 event=consume lane=0 seq=2 end=500000
 event=consume lane=0 seq=2 end=11000000
 event=consume lane=0 seq=2 end=201000000
 event=consume lane=0 seq=3 end=300000000
"""
        self.assertEqual(frame_ages(log), {"count": 2, "p50": 10, "p95": 200, "max": 200})

    def test_no_matches_are_not_zero_latency(self):
        self.assertEqual(frame_ages("event=consume lane=0 seq=2 end=5"), {"count": 0})


if __name__ == "__main__":
    unittest.main()
