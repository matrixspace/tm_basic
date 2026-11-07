#!/usr/bin/env python3
"""Compare isolated linear-pipeline latency, CPU cost and pacing errors."""
import argparse
import subprocess
import json
import pathlib
import statistics
import hashlib
import random
parser = argparse.ArgumentParser(description='Process-isolated end-to-end executor latency comparison')
parser.add_argument('--baseline', type=pathlib.Path)
parser.add_argument('--candidate-only', action='store_true')
parser.add_argument('--include-legacy', action='store_true')
parser.add_argument('--engines', default='inline,dedicated,pool,moody', help='comma-separated pipeline engines, including legacy when selected explicitly')
parser.add_argument('--warmup', type=int, default=2000)
parser.add_argument('--candidate', type=pathlib.Path, required=True)
parser.add_argument('--output', type=pathlib.Path, required=True)
parser.add_argument('--repeats', type=int, default=5)
parser.add_argument('--messages', type=int, default=20000)
parser.add_argument('--sparse-messages', type=int, default=5000)
parser.add_argument('--producer-cpu', type=int, default=20)
parser.add_argument('--consumer-cpu', type=int, default=24)
parser.add_argument('--consumer-cpus', default='')
parser.add_argument('--cpus', default='20-27')
parser.add_argument('--hops', default='1,4')
parser.add_argument('--intervals', default='0,50')
parser.add_argument('--spins', default='0,50,200')
parser.add_argument('--mode', choices=['closed', 'periodic'], default='closed')
parser.add_argument('--idle-baseline', type=pathlib.Path)
parser.add_argument('--idle-candidate', type=pathlib.Path)
opt = parser.parse_args()
if bool(opt.idle_baseline) != bool(opt.idle_candidate):
    parser.error('idle baseline and candidate must be supplied together')
if opt.repeats < 1 or opt.messages < 1 or opt.sparse_messages < 1 or opt.warmup < 1:
    parser.error('counts must be positive')
hops_values = [int(v) for v in opt.hops.split(',')]
interval_values = [int(v) for v in opt.intervals.split(',')]
spin_values = sorted(set([0] + [int(v) for v in opt.spins.split(',')]))
if not opt.candidate_only and not opt.baseline:
    parser.error('--baseline is required unless --candidate-only')
engines = list(dict.fromkeys(opt.engines.split(',')))
if opt.include_legacy and 'legacy' not in engines:
    engines.append('legacy')
if not engines or any(e not in ('inline', 'dedicated', 'pool', 'moody', 'legacy') for e in engines):
    parser.error('invalid --engines list')
out = opt.output
out.mkdir(parents=True, exist_ok=True)
bins = {'candidate': opt.candidate.resolve()} if opt.candidate_only else {'baseline': opt.baseline.resolve(), 'candidate': opt.candidate.resolve()}
sha = {v: hashlib.sha256(p.read_bytes()).hexdigest() for v, p in bins.items()}
rows = []
with (out / 'results.jsonl').open('w') as f:
    for repeat in range(opt.repeats):
        for hops in hops_values:
            for interval in interval_values:
                for engine in engines:
                    choices = ([] if opt.candidate_only else [('baseline', 0)]) + [('candidate', 0)] + ([] if engine in ('inline', 'legacy') else [('candidate', s) for s in spin_values if s])
                    random.Random(repeat).shuffle(choices)
                    for version, spin in choices:
                        n = opt.messages if interval == 0 else opt.sparse_messages
                        args = ['--executor', engine, '--hops', str(hops), '--messages', str(n), '--interval-us', str(interval), '--spin-us', str(spin), '--producer-cpu', str(opt.producer_cpu), '--consumer-cpu', str(opt.consumer_cpu), '--mode', opt.mode, '--warmup', str(opt.warmup)] + (['--consumer-cpus', opt.consumer_cpus] if opt.consumer_cpus else [])
                        p = subprocess.run((['taskset', '-c', opt.cpus] if opt.cpus else []) + [str(bins[version]), *args], text=True, capture_output=True, timeout=90)
                        if p.returncode:
                            raise RuntimeError(str(args) + p.stderr)
                        r = json.loads(p.stdout)
                        if not r['ok']:
                            raise RuntimeError(str(r))
                        r.update(version=version, repeat=repeat, binary_sha256=sha[version], warmup=opt.warmup)
                        rows.append(r)
                        f.write(json.dumps(r, sort_keys=True) + '\n')
                        f.flush()
                    print(repeat, hops, interval, engine, flush=True)
with (out / 'summary.md').open('w') as f:
    f.write('| Executor | Hops | interval μs | Version / spin μs | p50 ns | p99 ns | CPU ns/item |\n|---|---:|---:|---|---:|---:|---:|\n')
    for hops in hops_values:
        for interval in interval_values:
            for engine in engines:
                for version, spin in ([] if opt.candidate_only else [('baseline', 0)]) + [('candidate', s) for s in spin_values]:
                    group = [r for r in rows if (r['executor'], r['hops'], r['interval_us'], r['version'], r['spin_us']) == (engine, hops, interval, version, spin)]
                    if not group:
                        continue
                    vals = [statistics.median([r[k] for r in group]) for k in ['p50_ns', 'p99_ns']] + [statistics.median([r['process_cpu_ns'] / r['messages'] for r in group])]
                    f.write('| ' + f'{engine} | {hops} | {interval} | {version} / {spin} | ' + ' | '.join((f'{v:.0f}' for v in vals)) + ' |\n')
print(out / 'summary.md', flush=True)

if opt.idle_baseline:
    idle_rows = []
    idle_bins = {'baseline': opt.idle_baseline.resolve(), 'candidate': opt.idle_candidate.resolve()}
    with (out / 'idle.jsonl').open('w') as f:
        for repeat in range(opt.repeats):
            for engine in [e for e in engines if e != 'inline']:
                for active in [False, True]:
                    versions = list(idle_bins)
                    random.Random(repeat).shuffle(versions)
                    for version in versions:
                        binary = idle_bins[version]
                        args = ['--executor', engine, '--scenario', 'idle', '--nodes', '128', '--idle-ms', '300',
                                '--producer-cpus', str(opt.producer_cpu), '--consumer-cpus', opt.consumer_cpus or ','.join(str(opt.consumer_cpu+i) for i in range(4))]
                        if active:
                            args.append('--idle-work')
                        result = subprocess.run((['taskset', '-c', opt.cpus] if opt.cpus else []) + [str(binary), *args], text=True, capture_output=True, timeout=90)
                        if result.returncode:
                            raise RuntimeError(result.stderr)
                        record = json.loads(result.stdout)
                        if not record['ok']:
                            raise RuntimeError(str(record))
                        record.update(version=version, repeat=repeat, binary_sha256=hashlib.sha256(binary.read_bytes()).hexdigest())
                        idle_rows.append(record)
                        f.write(json.dumps(record, sort_keys=True)+'\n')
                        f.flush()
    with (out / 'idle-summary.md').open('w') as f:
        f.write('| Executor | Active idle | Version | CPU ms/second |\n|---|---|---|---:|\n')
        for engine in [e for e in engines if e != 'inline']:
            for active in [False, True]:
                for version in idle_bins:
                    group = [r for r in idle_rows if (r['executor'], r['idle_work'], r['version']) == (engine, active, version)]
                    cpu = statistics.median(r['process_cpu_ns']/r['wall_ns']*1000 for r in group)
                    f.write(f'| {engine} | {active} | {version} | {cpu:.3f} |\n')
