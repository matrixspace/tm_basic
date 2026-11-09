#!/usr/bin/env python3
"""Compare moodycamel executor binaries; keep allocation hooks out of timing."""
import argparse
import hashlib
import json
from pathlib import Path
import statistics
import subprocess


def cases():
    for producers in (1, 4):
        for payload in (0, 256):
            yield f'throughput-p{producers}-b{payload}', ['--messages', '1000000', '--producers', str(producers), '--payload', str(payload)]
            yield f'latency-p{producers}-b{payload}', ['--scenario', 'latency', '--messages', '10000', '--producers', str(producers), '--payload', str(payload)]
    yield 'sparse', ['--scenario', 'latency', '--messages', '3000', '--interval-us', '50']
    yield 'idle', ['--scenario', 'idle', '--nodes', '32', '--idle-ms', '300']


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for arg in ('baseline', 'candidate', 'baseline-counts', 'candidate-counts', 'output'):
        parser.add_argument('--'+arg, type=Path, required=True)
    parser.add_argument('--executor', default='moody')
    parser.add_argument('--repeats', type=int, default=5)
    opt = parser.parse_args()
    opt.output.mkdir(parents=True, exist_ok=True)
    binaries = {version+suffix: getattr(opt, version+suffix.replace('-', '_'))
                for version in ('baseline', 'candidate') for suffix in ('', '-counts')}
    hashes = {name: hashlib.sha256(path.read_bytes()).hexdigest() for name, path in binaries.items()}
    rows = []
    with (opt.output/'results.jsonl').open('w') as output:
        for name, args in cases():
            for counted in (False, True):
                for repeat in range(1 if counted else opt.repeats):
                    for version in (('baseline', 'candidate') if repeat % 2 == 0 else ('candidate', 'baseline')):
                        key = version+('-counts' if counted else '')
                        cmd = ['taskset', '-c', '20-27', str(binaries[key]), '--executor', opt.executor,
                               '--producer-cpus', '20,21,22,23', '--consumer-cpus', '24,25,26,27', *args]
                        result = subprocess.run(cmd, capture_output=True, text=True, timeout=90, check=True)
                        row = json.loads(result.stdout)
                        if not row['ok']:
                            raise RuntimeError(row)
                        row.update(case=name, version=version, repeat=repeat, binary_sha256=hashes[key], command=cmd)
                        rows.append(row)
                        output.write(json.dumps(row)+'\n')
                        output.flush()
            print(name+' complete', flush=True)
    with (opt.output/'summary.md').open('w') as output:
        output.write(f'# Moodycamel executor comparison\n\nGCC 15, C++17, `-O3 -pthread`; {opt.repeats} alternating runs; CPU 20–23 producers, 24–27 consumer. CPU includes coordination; sparse pacing is part of the producer measurement. Counts are separate runs.\n\n')
        output.write('| Case | CPU ns/item before→after | wall ns/item before→after | p50 ns before→after | p99 ns before→after | allocations before→after |\n|---|---:|---:|---:|---:|---:|\n')
        for name, _ in cases():
            results = []
            for version in ('baseline', 'candidate'):
                timed = [r for r in rows if r['case'] == name and r['version'] == version and not r['instrumented']]
                counted = [r for r in rows if r['case'] == name and r['version'] == version and r['instrumented']][0]
                results.append([statistics.median(r['process_cpu_ns']/max(1, r['items']) for r in timed),
                                statistics.median(r['wall_ns']/max(1, r['items']) for r in timed),
                                statistics.median(r['p50_ns'] for r in timed), statistics.median(r['p99_ns'] for r in timed), counted['allocations']])
            values = [' → '.join(f'{r[i]:.0f}' for r in results) for i in range(5)]
            output.write('| '+name+' | '+' | '.join(values)+' |\n')
        output.write('\nIdle rows show total CPU/wall ns per run (zero items), not ns/item. Allocations are counts from the entire instrumented measurement window, not retained heap. Tail estimates use limited samples on a shared machine.\n')


if __name__ == '__main__':
    main()
