#!/usr/bin/env python3
"""Measure the existing H.264 path; capture/encode and app display are excluded."""
import argparse
import bisect
import json
import math
import re
import socket
import subprocess
import sys
import threading
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'desktop'))
from recam_ui.device import Device, REMOTE_DIR, STREAM_PORT
from recam_ui.streamer import build_command


def distribution(values):
    values = sorted(values)
    if not values:
        return {'count': 0}
    def percentile(p):
        return values[max(0, math.ceil(p * len(values)) - 1)]
    return dict(count=len(values), p50=percentile(.5), p95=percentile(.95),
                p99=percentile(.99), max=values[-1])


def clock_sample(device, count=12):
    samples = []
    for _ in range(count):
        start = time.monotonic_ns()
        status = device.status()
        end = time.monotonic_ns()
        if not status:
            raise RuntimeError('Cannot sample device clock')
        stamp = status['now_ns']
        samples.append({'lower_ns': stamp - end, 'upper_ns': stamp - start,
                        'rtt_ns': end - start})
    # The clock read occurred between request and reply. No path symmetry assumed.
    lower = max(s['lower_ns'] for s in samples)
    upper = min(s['upper_ns'] for s in samples)
    if lower > upper:
        raise RuntimeError('Inconsistent clock samples')
    return dict(lower_ns=lower, upper_ns=upper, samples=samples)


def analyze(log, chunks, clocks, warmup=3, duration=None):
    publications, consumes = {}, []
    decoder_stats = []
    for line in log.splitlines():
        stat = re.search(r'^\s*(\d+\.\d+).*stream pictures in=(\d+) out=(\d+) shown=(\d+), (\d+) bytes queued', line)
        if stat:
            decoder_stats.append([float(stat[1]), *map(int, stat.groups()[1:])])
        match = re.search(r'event=(publish|consume)\s+(.*)', line)
        if not match:
            continue
        row = {k: int(v) for k, v in re.findall(r'(\w+)=(\d+)', match[2])}
        if match[1] == 'publish':
            publications[(row['lane'], row['seq'])] = row
        else:
            consumes.append(row)
    if not publications:
        raise RuntimeError('No publications measured. Open a supported camera preview.')
    first = min(p['receive'] for p in publications.values())
    window_start = first + warmup * 1e9
    # Stop one second before the nominal source end, excluding drain/stale reuse
    # during the post-run clock samples. Startup uses publication time.
    window_end = first + (duration - 1) * 1e9 if duration else float('inf')
    publications = {k: p for k, p in publications.items()
                    if window_start <= p['publish'] <= window_end}
    metrics = {k: [] for k in ('receive_to_queue_ms', 'queue_to_dequeue_ms',
               'output_copy_ms', 'scale_ms', 'receive_to_publish_ms',
               'publish_to_consume_ms', 'camera_copy_ms', 'receive_to_consume_ms',
               'host_to_consume_estimate_ms', 'host_to_consume_lower_ms',
               'host_to_consume_upper_ms', 'host_to_receive_estimate_ms')}
    # Envelope covers pre/post clock-offset samples; not a claim of zero drift.
    lower = min(c['lower_ns'] for c in clocks)
    upper = max(c['upper_ns'] for c in clocks)
    midpoint = (lower + upper) / 2
    for p in publications.values():
        for metric, a, b in [('receive_to_queue_ms','receive','queue'),
                             ('queue_to_dequeue_ms','queue','decode'),
                             ('output_copy_ms','decode','copy'),
                             ('scale_ms','scale','publish'),
                             ('receive_to_publish_ms','receive','publish')]:
            if p[b] >= p[a] > 0:
                metrics[metric].append((p[b] - p[a]) / 1e6)
    ends = [c['end'] for c in chunks]
    for p in publications.values():
        index = bisect.bisect_right(ends, p['wire'])
        if index < len(chunks):
            metrics['host_to_receive_estimate_ms'].append(
                (p['receive'] - chunks[index]['ns'] - midpoint) / 1e6)
    seen, unmatched = set(), 0
    for c in consumes:
        if not window_start <= c['end'] <= window_end:
            continue
        key = (c['lane'], c['seq'])
        p = publications.get(key)
        if not p:
            unmatched += 1
            continue
        if not p['publish'] <= c['start'] <= c['end']:
            continue
        seen.add(key)
        metrics['publish_to_consume_ms'].append((c['start']-p['publish'])/1e6)
        metrics['camera_copy_ms'].append((c['end']-c['start'])/1e6)
        metrics['receive_to_consume_ms'].append((c['end']-p['receive'])/1e6)
        index = bisect.bisect_right(ends, p['wire'])
        if index < len(chunks):
            host = chunks[index]['ns']
            delta = c['end'] - host
            metrics['host_to_consume_estimate_ms'].append((delta-midpoint)/1e6)
            metrics['host_to_consume_lower_ms'].append((delta-upper)/1e6)
            metrics['host_to_consume_upper_ms'].append((delta-lower)/1e6)
    decoded_fps = None
    # Exclude codec startup. Log times are used only for within-device intervals.
    if len(decoder_stats) >= 3:
        a, b = decoder_stats[1], decoder_stats[-1]
        if b[0] > a[0]: decoded_fps = (b[2] - a[2]) / (b[0] - a[0])
    return dict(metrics={k: distribution(v) for k,v in metrics.items()},
                decoder_output_fps=decoded_fps, decoder_stats=decoder_stats,
                measurement_window_ns=[int(window_start), int(window_end) if duration else None],
                clock_uncertainty_ms=(upper-lower)/2e6,
                clock_offset_envelope_ns=[lower,upper],
                publications=len(publications), unique_consumed=len(seen),
                publications_not_observed_consumed=len(publications)-len(seen),
                unmatched_consumes_including_warmup=unmatched,
                lanes=sorted({(p['width'],p['height']) for p in publications.values()}))


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--serial', required=True)
    ap.add_argument('--seconds', type=int, default=30)
    ap.add_argument('--size', default='1280x720')
    ap.add_argument('--output', type=Path, required=True)
    ap.add_argument('--transport', choices=['flv', 'annexb'], default='flv')
    a = ap.parse_args()
    if a.seconds < 6 or not re.fullmatch(r'[1-9]\d*x[1-9]\d*', a.size):
        ap.error('Use at least 6 seconds and a WIDTHxHEIGHT size')
    a.output.mkdir(parents=True, exist_ok=False)
    device = Device(a.serial)
    initial = device.status()
    if not initial or initial.get('payload', {}).get('active') or initial['feeder']['running']:
        raise RuntimeError('Stop existing ReCam activity before profiling')
    print(device.ensure_deployed(), flush=True)
    logcat = encoder = None
    proxy = None
    forwarded = None
    chunks, errors = [], []
    stop = threading.Event()
    listener = socket.socket()
    listener.bind(('127.0.0.1', 0))
    listener.listen(1)
    listener.settimeout(.5)
    local_port = listener.getsockname()[1]
    try:
        device.inject()
        ready_deadline = time.monotonic() + 10
        while not (device.status() or {}).get('ring', {}).get('lanes'):
            if time.monotonic() >= ready_deadline:
                raise RuntimeError('No active camera buffers; unlock and open Camera before profiling')
            time.sleep(.2)
        before = clock_sample(device)
        with (a.output/'device.log').open('w') as log, (a.output/'ffmpeg.log').open('w') as fferr:
            logcat = subprocess.Popen(device._argv(['logcat','-v','epoch','-T','1',
                                       '-s','ReCamLatency:I','KittyMemoryEx:I','*:S']), stdout=log, stderr=subprocess.PIPE)
            rc,out,err = device.shell(f'RECAM_PROFILE=1 nohup {REMOTE_DIR}/recam feed '
                                     '>/data/local/tmp/recam_profile_feed.log 2>&1 &')
            if rc: raise RuntimeError(err or out)
            rc,out,err = device._run(['forward','tcp:0',f'tcp:{STREAM_PORT}'])
            if rc: raise RuntimeError(err or out)
            forwarded = int(out)

            def relay():
                try:
                    while not stop.is_set():
                        try:
                            incoming, _ = listener.accept()
                            break
                        except socket.timeout:
                            continue
                    else: return
                    with incoming:
                        incoming.settimeout(.5)
                        with socket.create_connection(('127.0.0.1',forwarded), timeout=5) as target:
                            offset=0
                            while not stop.is_set():
                                try: data = incoming.recv(65536)
                                except socket.timeout: continue
                                if not data: break
                                stamp=time.monotonic_ns()
                                offset += len(data)
                                chunks.append(dict(end=offset, ns=stamp))
                                target.sendall(data)
                except Exception as e:
                    errors.append(str(e))
            # Wait for the feeder to bind before ffmpeg connects.
            time.sleep(1)
            proxy=threading.Thread(target=relay, daemon=True)
            proxy.start()
            command=build_command('ffmpeg', ['-re','-f','lavfi','-i',
                       f'testsrc2=size={a.size}:rate=30','-t',str(a.seconds)], local_port, transport=a.transport)
            (a.output/'command.json').write_text(json.dumps(command,indent=2))
            encoder=subprocess.Popen(command, stdout=subprocess.DEVNULL, stderr=fferr)
            print(f'Measuring {a.size} at 30 fps for {a.seconds}s; keep camera preview open',flush=True)
            encoder.wait(timeout=a.seconds+30)
            if encoder.returncode: raise RuntimeError('FFmpeg failed; see ffmpeg.log')
            proxy.join(timeout=5)
            if proxy.is_alive(): raise RuntimeError('Proxy did not finish')
            if errors: raise RuntimeError(errors)
            time.sleep(.5)
            after=clock_sample(device)
            final=device.status()
            device.stop_feeder()
            logcat.terminate()
            logcat.wait(timeout=5)
        (a.output/'host_chunks.json').write_text(json.dumps(chunks))
        (a.output/'clocks.json').write_text(json.dumps([before,after],indent=2))
        report=analyze((a.output/'device.log').read_text(),chunks,[before,after],duration=a.seconds)
        report.update(size=a.size, transport=a.transport, fps=30, seconds=a.seconds, warmup_seconds=3,
                      clock_samples=[before,after], initial_status=initial, final_status=final,
                      endpoint='host encoded-byte proxy ingress to completed camera-plane copy',
                      excluded=['capture and encoding','camera buffer unlock','app delivery and display'],
                      caveats=['proxy and per-frame logging add measurement overhead',
                               'queue-to-dequeue includes feeder polling, not just hardware decode',
                               'receive timestamp is userspace read, not kernel packet arrival',
                               'clock bound assumes no offset excursion beyond pre/post envelope'])
        (a.output/'report.json').write_text(json.dumps(report,indent=2)+'\n')
        print(json.dumps({k:v for k,v in report.items() if k in
              ('metrics','clock_uncertainty_ms','lanes','publications','unique_consumed')},indent=2))
        if not report['unique_consumed']:
            raise RuntimeError('No camera consumption measured; open camera preview and repeat')
    finally:
        stop.set()
        listener.close()
        for proc in (encoder,logcat):
            if proc and proc.poll() is None:
                proc.terminate()
                try: proc.wait(timeout=5)
                except subprocess.TimeoutExpired: proc.kill(); proc.wait()
        if proxy: proxy.join(timeout=6)
        try: device.stop_feeder()
        finally:
            try: device.stop_payload()
            finally:
                if forwarded: device.remove_forward(forwarded)

if __name__ == '__main__':
    main()
