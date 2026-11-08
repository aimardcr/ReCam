import sys
import unittest
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from recam_ui.device import Device, DeviceError


class PayloadErrors(unittest.TestCase):
    def setUp(self):
        self.device = Device(adb="adb")

    def test_payload_failures_reach_the_ui_job(self):
        for command in (self.device.inject, self.device.stop_payload):
            with self.subTest(command=command.__name__):
                with patch.object(self.device, "shell", return_value=(1, "", "")):
                    with self.assertRaisesRegex(DeviceError, "failed .*exit 1.*logcat"):
                        command()

    def test_failure_preserves_device_diagnostics(self):
        with patch.object(self.device, "shell", return_value=(7, "stdout", "stderr")):
            with self.assertRaises(DeviceError) as raised:
                self.device.inject()
        message = str(raised.exception)
        for detail in ("recam inject", "exit 7", "stdout", "stderr"):
            self.assertIn(detail, message)

    def test_success_preserves_the_existing_result_contract(self):
        for command in (self.device.inject, self.device.stop_payload):
            with self.subTest(command=command.__name__):
                result = (0, "done", "")
                with patch.object(self.device, "shell", return_value=result):
                    self.assertEqual(command(), result)


if __name__ == "__main__":
    unittest.main()
