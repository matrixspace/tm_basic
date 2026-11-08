#!/usr/bin/env python3
"""Randomized dual-baseline comparison for noisy multi-producer throughput."""
import argparse
import hashlib
import json
from pathlib import Path
import random
import statistics
import subprocess


def summarize(rows, seed):
    labels = ['baseline-a', 'baseline-b', 'candidate']
    rounds = len({r['repeat'] for r in rows})
    if rounds < 4 or len(rows) != rounds * 3 or not all(r['ok'] and not r['instrumented'] for r in rows):
        raise ValueError('need at least four complete, correct, uninstrumented rounds')
    summary = {}
    for label in labels:
        values = [r['process_cpu_ns'] / r['items'] for r in rows if r['label'] == label]
        quartiles = statistics.quantiles(values, n=4)
        summary[label] = dict(mean_cpu_ns=statistics.mean(values), median_cpu_ns=statistics.median(values),
                              q25=quartiles[0], q75=quartiles[2])
    rng = random.Random(seed)
    pairs = []
    for repeat in range(rounds):
        order = labels.copy()
        rng.shuffle(order)  # Reproduce the runner RNG state before bootstrapping.
        group = [r for r in rows if r['repeat'] == repeat]
        if len(group) != 3 or {r['label'] for r in group} != set(labels):
            raise ValueError('incomplete or duplicate round')
        by_label = {r['label']: r for r in group}
        if by_label['baseline-a']['binary_sha256'] != by_label['baseline-b']['binary_sha256']:
            raise ValueError('baseline controls must use the same binary')
        cpu = {name: r['process_cpu_ns'] / r['items'] for name, r in by_label.items()}
        pairs.append((cpu['candidate'], (cpu['baseline-a'] + cpu['baseline-b']) / 2))
    ratios = []
    for _ in range(10000):
        sample = rng.choices(pairs, k=rounds)
        ratios.append(sum(x for x, y in sample) / sum(y for x, y in sample) - 1)
    ratios.sort()
    summary['candidate_mean_change'] = sum(x for x, y in pairs) / sum(y for x, y in pairs) - 1
    summary['paired_bootstrap_95pct'] = [ratios[249], ratios[9749]]
    return summary


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--baseline', type=Path)
    parser.add_argument('--candidate', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--summarize-only', type=Path, help='replay an existing results.jsonl without running binaries')
    parser.add_argument('--rounds', type=int, default=30)
    parser.add_argument('--seed', type=int, default=20261108)
    parser.add_argument('--messages', type=int, default=1000000)
    parser.add_argument('--producers', type=int, default=4)
    parser.add_argument('--cpus', default='20-27')
    parser.add_argument('--producer-cpus', default='20,21,22,23')
    parser.add_argument('--consumer-cpus', default='24,25,26,27')
    opt = parser.parse_args()
    if opt.rounds < 4 or opt.messages < 1 or opt.producers < 1:
        parser.error('rounds must be >=4; counts must be positive')
    if not opt.summarize_only and (not opt.baseline or not opt.candidate):
        parser.error('baseline and candidate required unless --summarize-only')
    opt.output.mkdir(parents=True, exist_ok=True)
    if opt.summarize_only:
        rows = [json.loads(line) for line in opt.summarize_only.read_text().splitlines()]
    else:
        rows = []
        rng = random.Random(opt.seed)
        with (opt.output / 'results.jsonl').open('w') as output:
            for repeat in range(opt.rounds):
                labels = ['baseline-a', 'baseline-b', 'candidate']
                rng.shuffle(labels)
                for label in labels:
                    binary = (opt.candidate if label == 'candidate' else opt.baseline).resolve()
                    cmd = ['taskset', '-c', opt.cpus, str(binary), '--executor', 'enhanced',
                           '--producers', str(opt.producers), '--messages', str(opt.messages),
                           '--producer-cpus', opt.producer_cpus, '--consumer-cpus', opt.consumer_cpus]
                    row = json.loads(subprocess.check_output(cmd, text=True, timeout=90))
                    if not row['ok'] or row['instrumented']:
                        raise ValueError('incorrect result or instrumented timing binary')
                    row.update(label=label, repeat=repeat, command=cmd,
                               binary_sha256=hashlib.sha256(binary.read_bytes()).hexdigest())
                    rows.append(row)
                    output.write(json.dumps(row) + '\n')
                    output.flush()
                print('rounds', repeat + 1, flush=True)
    summary = summarize(rows, opt.seed)
    (opt.output / 'summary.json').write_text(json.dumps(summary, indent=2) + '\n')
    print(json.dumps(summary), flush=True)


if __name__ == '__main__':
    main()
