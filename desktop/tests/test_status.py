import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from recam_ui.status import lane_summary, diagnostic_summary


class StatusDiagnostics(unittest.TestCase):
    def test_stopped_feeder_is_not_reported_as_served(self):
        text = lane_summary(dict(req_width=1960, req_height=882, width=1920,
                                 height=864, scaled=True, state="feeder_stopped", age_ms=-1))
        self.assertIn("1960x882", text)
        self.assertIn("scaled from 1920x864", text)
        self.assertIn("feeder stopped", text)
        self.assertNotIn("-1", text)

    def test_legacy_status_still_renders(self):
        self.assertIn("served", lane_summary(dict(width=640, height=480, served=True, age_ms=10)))

    def test_unsupported_format_is_historical_and_named(self):
        text = diagnostic_summary({"diagnostics": {"unsupported_format": 20,
                                  "last_unsupported_format": 0x7FA30C06, "fallback_frames": 4}})
        self.assertIn("Qualcomm UBWC", text)
        self.assertIn("20 frames passed through", text)
        self.assertIn("since injection", text)


if __name__ == "__main__":
    unittest.main()
