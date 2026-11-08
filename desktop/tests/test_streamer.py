import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from recam_ui.streamer import build_command


class Framing(unittest.TestCase):
    def test_default_is_flushed_flv_without_frame_reordering(self):
        cmd = build_command('ffmpeg', ['-i', 'test'], 1234)
        self.assertEqual(cmd[cmd.index('-bf') + 1], '0')
        self.assertEqual(cmd[cmd.index('-f') + 1], 'flv')
        self.assertEqual(cmd[cmd.index('-flush_packets') + 1], '1')
        self.assertEqual(cmd[-1], 'tcp://127.0.0.1:1234')

    def test_legacy_annexb_is_available_for_comparison(self):
        cmd = build_command('ffmpeg', ['-i', 'test'], 1234, transport='annexb')
        self.assertEqual(cmd[cmd.index('-f') + 1], 'h264')
        self.assertNotIn('-flvflags', cmd)

    def test_unknown_transport_is_rejected(self):
        with self.assertRaises(ValueError):
            build_command('ffmpeg', [], 1234, transport='bogus')
