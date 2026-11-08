import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from profile_compositor import age_bounds


class SnapshotBounds(unittest.TestCase):
    def test_generated_before_request(self):
        self.assertEqual(age_bounds(1_000_000, 11_000_000, 31_000_000),
                         {"lower_ms": 10, "upper_ms": 30, "screenshot_round_trip_ms": 20})

    def test_generated_during_capture(self):
        self.assertEqual(age_bounds(21_000_000, 11_000_000, 31_000_000)["lower_ms"], 0)

    def test_header_bound_excludes_remaining_transfer(self):
        self.assertEqual(age_bounds(1_000_000, 11_000_000, 31_000_000, 101_000_000),
                         {"lower_ms": 10, "upper_ms": 30, "screenshot_round_trip_ms": 90})

    def test_impossible_timestamp_is_rejected(self):
        with self.assertRaises(ValueError):
            age_bounds(40, 10, 30)
        with self.assertRaises(ValueError):
            age_bounds(1, 40, 30)
