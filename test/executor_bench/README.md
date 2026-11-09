# Executor benchmarks

Use the worktree's own Release build after sourcing build-common/build-env-setting.sh.
Timing targets and allocation-counting targets are separate. Engine IDs are:
1 direct, 2 inline, 3 inline-erased, 4 dedicated, 5 pool, 6 moody.
`executor_bench_moody` and `_counts` are only configured when moodycamel is available.
The aggregate and pipeline binaries remain buildable without it; unavailable engines
are rejected at runtime. The former enhanced engine and its dedicated targets are removed.

```sh
ninja -j2 -C builddir_release \
  test/executor_bench/executor_bench_moody \
  test/executor_bench/executor_bench_moody_counts \
  test/executor_bench/executor_latency_bench
builddir_release/test/executor_bench/executor_bench_moody \
  --executor moody --producers 4 --messages 1000000
builddir_release/test/executor_bench/executor_latency_bench \
  --executor moody --hops 4 --messages 10000 --warmup 100 --spin-us 200
```

ExecutorBench supports throughput/latency/idle, payload size, producer/node counts,
CPU pinning, pacing and separate allocation/move instrumentation. See `--help`.
ExecutorLatencyBench measures multihop end-to-end timing, using closed-loop or
periodic inputs, and reports correctness plus CPU/wall/quantiles. Exact counts and
per-producer order are checked; cross-producer FIFO is not required for moody.
The tools use different messages and acknowledgements: do not mix their absolute p50.

`run_moodycamel_profile.py` compares baseline/candidate timing and counts binaries,
five alternating runs by default. Required arguments: --baseline, --candidate,
--baseline-counts, --candidate-counts, --output. `--executor` defaults to moody.
`run_contention_profile.py` uses two identical baseline labels and one candidate
in randomized rounds; --baseline, --candidate and --output are required. It saves
SHA256/commands and bootstraps a paired mean CPU/item interval. Use --summarize-only
results.jsonl to reproduce statistics. --executor defaults to moody.
`run_latency.py` compares pipeline engines. All CPU defaults describe this machine;
adjust --producer-cpus/--consumer-cpus or equivalent for local topology.

To approach Inline on sparse inputs, try SMT sibling placement and a spin budget
longer than the input interval (e.g. --spin-us 2000 for --interval-us 1000). This can
consume one logical CPU; SMT shares execution resources with business callbacks.
With idle work, spinWait is capped by Config::idlePeriod. Defaults do not force
affinity or increase spin budget. Reusable 256-item blocks trade more partially
filled-block memory for throughput; retention depends on producer count and peak backlog.

Historical Enhanced/unpooled profiling scripts and CLI are available from prior
commits, alongside archived raw data in infra/doc. Current tooling uses only moody.
[Performance record](../../../tm_infra/doc/20261105-realtime-threaded-executor/unordered-enhanced.md)
and [unification](../../../tm_infra/doc/20261105-realtime-threaded-executor/moodycamel-unification.md).
