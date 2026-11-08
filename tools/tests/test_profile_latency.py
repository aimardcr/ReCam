import sys
import unittest
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from profile_latency import analyze, distribution


class ProfileAnalysis(unittest.TestCase):
    def test_nearest_rank_and_empty(self):
        self.assertEqual(distribution([]), {'count': 0})
        self.assertEqual(distribution([3,1,2,4])['p50'], 2)
        self.assertEqual(distribution([3,1,2,4])['p99'], 4)

    def test_frame_join_byte_boundary_and_clock_bounds(self):
        log = '''event=publish lane=0 seq=3 width=640 height=480 wire=10 receive=20000000 queue=30000000 decode=40000000 copy=41000000 scale=42000000 publish=45000000
 event=consume lane=0 seq=3 start=50000000 end=51000000
 event=consume lane=1 seq=3 start=50000000 end=51000000'''
        log += """
 100.0 I decoder: stream pictures in=0 out=0 shown=0, 0 bytes queued
 105.0 I decoder: stream pictures in=101 out=100 shown=99, 12 bytes queued
 110.0 I decoder: stream pictures in=251 out=250 shown=249, 12 bytes queued
 115.0 I decoder: stream pictures in=401 out=400 shown=399, 12 bytes queued
"""
        chunks = [{'end':10,'ns':1000000}, {'end':20,'ns':2000000}]
        clocks = [{'lower_ns':9000000,'upper_ns':11000000}]
        report=analyze(log,chunks,clocks,warmup=0)
        m=report['metrics']
        self.assertEqual(m['receive_to_queue_ms']['p50'],10)
        self.assertEqual(m['queue_to_dequeue_ms']['p50'],10)
        self.assertEqual(m['output_copy_ms']['p50'],1)
        self.assertEqual(m['scale_ms']['p50'],3)
        self.assertEqual(m['publish_to_consume_ms']['p50'],5)
        self.assertEqual(m['receive_to_consume_ms']['p50'],31)
        self.assertEqual(m['host_to_consume_estimate_ms']['p50'],39)
        self.assertEqual(m['host_to_consume_lower_ms']['p50'],38)
        self.assertEqual(m['host_to_consume_upper_ms']['p50'],40)
        self.assertEqual(report['decoder_output_fps'],30)
        self.assertEqual(report['unique_consumed'],1)
        self.assertEqual(report['unmatched_consumes_including_warmup'],1)

    def test_excludes_post_stream_stale_reuse(self):
        log = """event=publish lane=0 seq=1 width=640 height=480 wire=0 receive=1000000000 queue=1100000000 decode=1200000000 copy=1201000000 scale=1202000000 publish=2000000000
 event=consume lane=0 seq=1 start=2100000000 end=2101000000
 event=consume lane=0 seq=1 start=9000000000 end=9001000000"""
        r = analyze(log, [{'end':10,'ns':900000000}],
                    [{'lower_ns':0,'upper_ns':0}], warmup=0, duration=5)
        self.assertEqual(r['metrics']['receive_to_consume_ms']['count'],1)
        self.assertEqual(r['metrics']['receive_to_consume_ms']['p50'],1101)

    def test_no_data_is_not_zero_latency(self):
        with self.assertRaisesRegex(RuntimeError,'No publications'):
            analyze('',[],[],warmup=0)

if __name__ == '__main__':
    unittest.main()
