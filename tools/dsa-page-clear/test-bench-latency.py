#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Exercise run_bench.sh reporting with fake sysfs/sudo/hackbench; no DSA needed."""
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile


MOCK_BENCH = r'''#!/usr/bin/env python3
import os
from pathlib import Path
p = Path(os.environ['DSA_PAGE_CLEAR_PARAMS'])
mode = os.environ['MOCK_LATENCY_MODE']
steps = dict(latency_samples=2, latency_sampled_pages=16,
             latency_total_ns=10000, latency_submit_ns=1000,
             latency_poll_loops=10, latency_zero_polls=1, latency_errors=0)
for name, step in steps.items():
    f = p / name
    if f.exists():
        old = int(f.read_text())
        f.write_text(str(0 if mode == 'reset' else old + (step if mode == 'normal' else 0)) + '\n')
f = p / 'latency_histogram'
if f.exists():
    counts = list(map(int, f.read_text().split()))
    if mode == 'normal':
        counts[2] += 1
        counts[3] += 1
    elif mode == 'reset':
        counts = [0]*8
    f.write_text(' '.join(map(str, counts)) + '\n')
print('Time: 0.001 (mock)')
'''


def main():
    bench = (Path(sys.argv[1]) if len(sys.argv) > 1 else
             Path(__file__).resolve().parents[3] / 'run_bench.sh')
    source = bench.read_text()
    base_names = ('min_pages cache_control unsafe_async batch_size batch_wait_us '
                  'batch_capacity completed_pages submitted_pages pending_pages '
                  'batch_submissions batched_descriptors single_submissions '
                  'async_fallback_pages async_error_pages').split()
    latency_names = ('latency_samples latency_sampled_pages latency_total_ns '
                     'latency_submit_ns latency_poll_loops latency_zero_polls '
                     'latency_errors latency_max_ns latency_max_polls').split()
    for mode in ('normal', 'zero', 'reset', 'old', 'old_disabled'):
        with tempfile.TemporaryDirectory(prefix='dsa-latency-test-') as temp:
            root = Path(temp)
            params, bins = root / 'params', root / 'bin'
            params.mkdir()
            bins.mkdir()
            for name in base_names:
                (params / name).write_text('0\n')
            modern = mode not in ('old', 'old_disabled')
            if modern:
                (params / 'latency_sample_every').write_text('0\n')
                for name in latency_names:
                    (params / name).write_text('100\n' if mode == 'reset' else '0\n')
                (params / 'latency_histogram').write_text('0 0 0 0 0 0 0 0\n')
            # sudo is deliberately replaced, so this test never escalates.
            (bins / 'sudo').write_text('#!/bin/sh\nexec "$@"\n')
            (bins / 'hackbench').write_text(MOCK_BENCH)
            for executable in bins.iterdir():
                executable.chmod(0o755)
            test_source = source
            if mode == 'old_disabled':
                test_source = re.sub(r'^BENCH_LATENCY_SAMPLE_EVERY=.*$',
                                     'BENCH_LATENCY_SAMPLE_EVERY=0', source, flags=re.M)
            script = root / 'run_bench.sh'
            script.write_text(test_source)
            env = dict(os.environ, PATH=f'{bins}:{os.environ["PATH"]}',
                       DSA_PAGE_CLEAR_PARAMS=str(params),
                       DSA_DEVICES=str(root / 'no-dsa'),
                       PAGE_CLEAR_DRIVER=str(root / 'no-driver'),
                       MOCK_LATENCY_MODE=mode)
            result = subprocess.run(['bash', str(script)], env=env,
                                    capture_output=True, text=True)
            if mode == 'old':
                assert result.returncode != 0
                assert 'requires the rebuilt module' in result.stderr
                assert '(mock)' not in result.stdout
            else:
                assert result.returncode == 0, result.stderr + result.stdout
            if mode == 'normal':
                checks = [r'mean pages per sample\s+8.00',
                          r'mean submit -> observed\s+5.000 us',
                          r'mean submit call\s+0.500 us',
                          r'mean return -> observed\s+4.500 us',
                          r'mean unsuccessful polls\s+5.00',
                          r'complete at first poll\s+50.00%',
                          r'2\.\.4 us\s+\d+\s+50.00%',
                          r'4\.\.8 us\s+\d+\s+50.00%']
                for pattern in checks:
                    assert re.search(pattern, result.stdout), result.stdout
                assert 'counts differ' not in result.stdout
            if mode == 'zero':
                assert 'No samples:' in result.stdout
            if mode == 'reset':
                assert 'Invalid latency interval: counters decreased' in result.stdout
            print(f'PASS: {mode}')


if __name__ == '__main__':
    main()
