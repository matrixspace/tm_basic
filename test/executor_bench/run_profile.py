#!/usr/bin/env python3
"""Paired process-isolated profiles: JSONL, median Markdown, optional perf.

Separate timing and counts binaries; alternate pair order to reduce order bias.
"""
import argparse
import hashlib
import json
from pathlib import Path
import statistics
import subprocess


def cases(quick):
    for executor in ('direct', 'inline', 'inline-erased', 'dedicated', 'pool', 'moody'):
        for producers in (1, 4):
            for payload in (0, 256):
                yield f'{executor}-p{producers}-b{payload}', ['--executor', executor, '--messages', '20000' if quick else '300000',
                                                           '--producers', str(producers), '--payload', str(payload)]
    for executor in ('pool', 'dedicated', 'moody'):
        yield executor+'-strands', ['--executor', executor, '--messages', '20000' if quick else '100000',
                                    '--producers', '4', '--nodes', '32', '--sample-every', '127']
    for executor in ('inline', 'dedicated', 'pool', 'moody'):
        yield executor+'-latency', ['--executor', executor, '--scenario', 'latency', '--messages', '1000' if quick else '3000', '--interval-us', '50']
    for executor in ('pool', 'dedicated', 'moody'):
        for active in (False, True):
            yield f'{executor}-idle-{int(active)}', ['--executor', executor, '--scenario', 'idle', '--nodes', '32' if quick else '128',
                                                    '--idle-ms', '100' if quick else '300'] + (['--idle-work'] if active else [])


def run(binary, args, cpus, perf=None):
    cmd = [str(binary), *args]
    if cpus:
        cmd = ['taskset', '-c', cpus, *cmd]
    if perf:
        cmd = ['perf', 'stat', '-x', ';', '-o', str(perf), '-e',
               'task-clock,context-switches,cpu-migrations,page-faults,cycles,instructions,cache-misses,branches,branch-misses', '--', *cmd]
    result = subprocess.run(cmd, text=True, capture_output=True, timeout=90)
    if result.returncode:
        raise RuntimeError(f'{binary.name} {args}: exit {result.returncode}\n{result.stderr}')
    record = json.loads(result.stdout)
    if not record['ok']:
        raise RuntimeError(f'incorrect benchmark result: {record}')
    return record


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for arg in ('baseline', 'candidate', 'output'):
        parser.add_argument('--'+arg, required=True, type=Path)
    for arg in ('baseline-counts', 'candidate-counts'):
        parser.add_argument('--'+arg, type=Path)
    parser.add_argument('--repeats', type=int, default=5)
    parser.add_argument('--cpus', default='')
    parser.add_argument('--quick', action='store_true')
    parser.add_argument('--perf', action='store_true')
    opt = parser.parse_args()
    if opt.repeats < 1:
        parser.error('--repeats must be positive')
    opt.output.mkdir(parents=True, exist_ok=True)
    binaries = {'baseline': opt.baseline.resolve(), 'candidate': opt.candidate.resolve()}
    hashes = {v: hashlib.sha256(p.read_bytes()).hexdigest() for v, p in binaries.items()}
    rows = []
    with (opt.output/'results.jsonl').open('w') as output:
        def save(record):
            output.write(json.dumps(record, sort_keys=True)+'\n')
            output.flush()
        for name, args in cases(opt.quick):
            for rep in range(opt.repeats):
                for label in (('baseline', 'candidate') if rep % 2 == 0 else ('candidate', 'baseline')):
                    record = run(binaries[label], args, opt.cpus)
                    if record['instrumented']:
                        raise RuntimeError('timing comparisons require the normal binary')
                    record.update(case=name, version=label, repeat=rep, binary_sha256=hashes[label], requested_affinity=opt.cpus)
                    rows.append(record)
                    save(record)
            print(name, 'OK', flush=True)
        for label, binary in (('baseline', opt.baseline_counts), ('candidate', opt.candidate_counts)):
            if binary:
                for executor in ('direct', 'inline', 'dedicated', 'pool', 'moody'):
                    for payload in (0, 256):
                        record = run(binary.resolve(), ['--executor', executor, '--messages', '20000', '--producers', '4', '--payload', str(payload)], opt.cpus)
                        if not record['instrumented']:
                            raise RuntimeError('counting requires the counts binary')
                        record.update(case=f'{executor}-counts-b{payload}', version=label, repeat=0)
                        save(record)
        if opt.perf:
            for executor in ('inline', 'dedicated', 'pool', 'moody'):
                for label in binaries:
                    record = run(binaries[label], ['--executor', executor, '--messages', '2000000'], opt.cpus,
                                 opt.output/f'perf-{label}-{executor}.csv')
                    record.update(case=executor+'-perf', version=label, repeat=0, perf_scope='whole_process_including_setup_warmup_output')
                    save(record)
    with (opt.output/'summary.md').open('w') as summary:
        summary.write('| Case | wall ns/item baseline→candidate | CPU ns/item baseline→candidate | CPU change |\n|---|---:|---:|---:|\n')
        for name, _ in cases(opt.quick):
            group = {v: [r for r in rows if r['case'] == name and r['version'] == v] for v in binaries}
            def metric(v, key):
                return statistics.median(r[key]/(r['items'] or r['wall_ns']/1e9) for r in group[v])
            wall = [metric(v, 'wall_ns') for v in binaries]
            cpu = [metric(v, 'process_cpu_ns') for v in binaries]
            summary.write(f'| {name} | {wall[0]:.1f} → {wall[1]:.1f} | {cpu[0]:.1f} → {cpu[1]:.1f} | {(cpu[1]/cpu[0]-1)*100:+.1f}% |\n')
        summary.write('\nIdle rows report CPU ns/second and wall ns/second. See JSONL for producer/background CPU, '
                      'latency percentiles, context switches, allocation/move counts and correctness.\n')
    print(opt.output/'summary.md', flush=True)


if __name__ == '__main__':
    main()
