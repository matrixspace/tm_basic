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


def cases(quick, include_legacy=False):
    for executor in ('direct', 'inline', 'inline-erased', 'dedicated', 'pool', 'moody') + (('legacy',) if include_legacy else ()):
        for producers in (1, 4):
            for payload in (0, 256):
                yield f'{executor}-p{producers}-b{payload}', ['--executor', executor, '--messages', '20000' if quick else ('6000000' if executor in ('direct', 'inline', 'inline-erased') else '1000000'),
                                                           '--producers', str(producers), '--payload', str(payload)]
    for executor in ('pool', 'dedicated', 'moody') + (('legacy',) if include_legacy else ()):
        yield executor+'-strands', ['--executor', executor, '--messages', '20000' if quick else '100000',
                                    '--producers', '4', '--nodes', '32', '--sample-every', '127']
    for executor in ('inline', 'dedicated', 'pool', 'moody') + (('legacy',) if include_legacy else ()):
        yield executor+'-latency', ['--executor', executor, '--scenario', 'latency', '--messages', '1000' if quick else '3000', '--interval-us', '50']
    for executor in ('pool', 'dedicated', 'moody') + (('legacy',) if include_legacy else ()):
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
    for arg in ('candidate', 'output'):
        parser.add_argument('--'+arg, required=True, type=Path)
    parser.add_argument('--baseline', type=Path)
    parser.add_argument('--candidate-only', action='store_true')
    parser.add_argument('--include-legacy', action='store_true')
    for arg in ('baseline-counts', 'candidate-counts'):
        parser.add_argument('--'+arg, type=Path)
    parser.add_argument('--repeats', type=int, default=5)
    parser.add_argument('--cpus', default='')
    parser.add_argument('--producer-cpus', default='')
    parser.add_argument('--consumer-cpus', default='')
    parser.add_argument('--quick', action='store_true')
    parser.add_argument('--perf', action='store_true')
    parser.add_argument('--split', action='store_true', help='require independently compiled per-executor binaries')
    opt = parser.parse_args()
    if opt.repeats < 1:
        parser.error('--repeats must be positive')
    if not opt.candidate_only and not opt.baseline:
        parser.error('--baseline is required unless --candidate-only')
    opt.output.mkdir(parents=True, exist_ok=True)
    binaries = ({'candidate': opt.candidate.resolve()} if opt.candidate_only else
                {'baseline': opt.baseline.resolve(), 'candidate': opt.candidate.resolve()})
    placement = []
    if opt.producer_cpus:
        placement += ['--producer-cpus', opt.producer_cpus]
    if opt.consumer_cpus:
        placement += ['--consumer-cpus', opt.consumer_cpus]
    def execute(binary, args, perf=None):
        if opt.split:
            engine=args[args.index('--executor')+1].replace('-', '_')
            counts=binary.name.endswith('_counts')
            stem=binary.name[:-7] if counts else binary.name
            binary=binary.with_name(stem+'_'+engine+('_counts' if counts else ''))
            if not binary.is_file():
                raise RuntimeError(f'missing split binary: {binary}')
        result=run(binary, args+placement, opt.cpus, perf)
        result['binary_sha256']=hashlib.sha256(binary.read_bytes()).hexdigest()
        result['split_build']=opt.split
        return result
    rows = []
    with (opt.output/'results.jsonl').open('w') as output:
        def save(record):
            output.write(json.dumps(record, sort_keys=True)+'\n')
            output.flush()
        for name, args in cases(opt.quick, opt.include_legacy):
            for rep in range(opt.repeats):
                for label in (list(binaries) if rep % 2 == 0 else list(reversed(binaries))):
                    record = execute(binaries[label], args)
                    if record['instrumented']:
                        raise RuntimeError('timing comparisons require the normal binary')
                    record.update(case=name, version=label, repeat=rep, requested_affinity=opt.cpus)
                    rows.append(record)
                    save(record)
            print(name, 'OK', flush=True)
        for label, binary in (('baseline', opt.baseline_counts), ('candidate', opt.candidate_counts)):
            if binary and label in binaries:
                for executor in ('direct', 'inline', 'dedicated', 'pool', 'moody') + (('legacy',) if opt.include_legacy else ()):
                    for payload in (0, 256):
                        record = execute(binary.resolve(), ['--executor', executor, '--messages', '20000', '--producers', '4', '--payload', str(payload)])
                        if not record['instrumented']:
                            raise RuntimeError('counting requires the counts binary')
                        record.update(case=f'{executor}-counts-b{payload}', version=label, repeat=0)
                        save(record)
                for active in (False, True):
                    record = execute(binary.resolve(), ['--executor', 'pool', '--scenario', 'idle', '--nodes', '128', '--idle-ms', '300']
                                     + (['--idle-work'] if active else []))
                    record.update(case=f'pool-idle-counts-{int(active)}', version=label, repeat=0)
                    save(record)
        if opt.perf:
            for executor in ('inline', 'dedicated', 'pool', 'moody') + (('legacy',) if opt.include_legacy else ()):
                for label in binaries:
                    record = execute(binaries[label], ['--executor', executor, '--messages', '2000000'],
                                 opt.output/f'perf-{label}-{executor}.csv')
                    record.update(case=executor+'-perf', version=label, repeat=0, perf_scope='whole_process_including_setup_warmup_output')
                    save(record)
    with (opt.output/'summary.md').open('w') as summary:
        if opt.candidate_only:
            summary.write('| Case | wall ns/item | CPU ns/item |\n|---|---:|---:|\n')
        else:
            summary.write('| Case | wall ns/item baseline→candidate | CPU ns/item baseline→candidate | CPU change |\n|---|---:|---:|---:|\n')
        for name, _ in cases(opt.quick, opt.include_legacy):
            group = {v: [r for r in rows if r['case'] == name and r['version'] == v] for v in binaries}
            def metric(v, key):
                return statistics.median(r[key]/(r['items'] or r['wall_ns']/1e9) for r in group[v])
            wall = [metric(v, 'wall_ns') for v in binaries]
            cpu = [metric(v, 'process_cpu_ns') for v in binaries]
            if opt.candidate_only:
                summary.write(f'| {name} | {wall[0]:.1f} | {cpu[0]:.1f} |\n')
            else:
                summary.write(f'| {name} | {wall[0]:.1f} → {wall[1]:.1f} | {cpu[0]:.1f} → {cpu[1]:.1f} | {(cpu[1]/cpu[0]-1)*100:+.1f}% |\n')
        summary.write('\nIdle rows report CPU ns/second and wall ns/second. See JSONL for producer/background CPU, '
                      'latency percentiles, context switches, allocation/move counts and correctness.\n')
    print(opt.output/'summary.md', flush=True)


if __name__ == '__main__':
    main()
