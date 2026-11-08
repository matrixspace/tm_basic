# Executor profiling

Direct Executor/Host calls, without AppRunner, clocks in Environment, TimeChecker,
logging or business allocation. Existing threaded-handler tests remain the
correctness suite; simple_graph_bench remains the graph integration benchmark.

Build release targets `test/executor_bench/executor_bench` and
`test/executor_bench/executor_bench_counts`, after sourcing the workspace build
environment. Use `ninja -j2`; do not build the entire examples tree for profiling.

## Cases and metrics

- `--executor direct|inline|inline-erased|dedicated|pool|moody|legacy`.
  direct calls the same Host directly; inline uses an optional statically typed
  submission hook when available; inline-erased always uses `push(Item&&)`.
- `--scenario throughput|latency|idle`. One case per process; warm-up and
  construction are excluded. Hosts/executors stay alive until process exit to
  avoid the existing detached-thread teardown issue and cross-case interference.
- Throughput defaults to no timestamps. Optional `--sample-every N` enables
  sampled submit→callback latency; record this observer overhead separately.
- Latency is closed-loop per producer, timestamps every message and optionally
  sleeps `--interval-us N` between messages. Payload construction precedes the
  submit timestamp. Producer CPU includes acknowledgement polling/pacing work.
- Payloads `--payload 0|256` are move-only and not default-constructible. Printed
  item_bytes includes metadata/alignment. A compiler barrier retains storage;
  the callback does no full business checksum of the byte buffer.
- `--producers`, `--nodes`, `--pool-threads`, `--max-batch` control contention and
  strand fairness. External taskset sets the allowed CPU set for every thread;
  For controlled comparisons, use --producer-cpus 20,21,22,23 and
  --consumer-cpus 24,25,26,27 to pin each producer and dedicated consumer;
  pool workers use the consumer list through pool configuration. Lists are
  checked against the allowed affinity. CPU migration remains possible when
  explicit per-role placement is omitted and is visible to perf.
- Idle uses `--nodes N --idle-ms N [--idle-work]` after startup settles. It
  reports process CPU, context switches, idle callbacks and allocations.
- Throughput wall endpoint is the final expected callback. CPU endpoint follows
  producer joins, without an arbitrary quiet interval. Producer CPU is measured
  using CLOCK_THREAD_CPUTIME_ID around each producer's submission loop; process
  CPU uses CLOCK_PROCESS_CPUTIME_ID. background_cpu_ns is the process remainder,
  including coordinator and pool background CPU, not an exact consumer-only
  hardware attribution. Inline callback CPU belongs to producer CPU.
- Every run checks exact counts, per-producer sequence and aggregate checksum.
  Functional counters are per-producer, on separate cache lines; completion
  atomics occur once per producer/node, not once per message. Latency samples
  are preallocated and single-writer per producer/node. Timeouts return failure.

## Keep counting separate from timing

The normal binary has no global allocation overrides or move counter atomics.
The `_counts` binary intercepts C++ new/delete (including aligned and sized
forms) and uses GNU link wrappers for executable/static-object malloc/free
references. This catches the installed moodycamel default allocator, which
calls malloc directly. It does not intercept libc/shared-library-internal
allocation calls, custom mmap allocators, calloc or realloc; extend the hooks
if a future executor uses those paths. allocated_bytes means requested bytes,
not allocator bookkeeping or resident memory. Warm-up/startup and result output
are outside the enabled counting interval. Free calls may include objects allocated
during warm-up or producer startup; the counters are not live-heap accounting.

Instrumented runs cannot establish CPU/latency speedups. Their purpose is to
verify allocation/move mechanisms separately; atomic counting changes contention.

## Repeated comparison

Save unoptimized executables before modifying headers, then rebuild candidate:

```sh
python3 test/executor_bench/run_profile.py \
  --baseline /tmp/executor-before/executor_bench \
  --baseline-counts /tmp/executor-before/executor_bench_counts \
  --candidate builddir_release/test/executor_bench/executor_bench \
  --candidate-counts builddir_release/test/executor_bench/executor_bench_counts \
  --split --cpus 20-27 --producer-cpus 20,21,22,23 --consumer-cpus 24,25,26,27 --repeats 5 --perf --output /tmp/executor-comparison
```

The full runner uses longer runs for cheap synchronous cases to reduce
frequency/startup noise. The runner alternates pair order, starts a fresh process per case, records
binary SHA256 and affinity, rejects failed/incorrect results, and retains
JSONL plus median Markdown. `--quick` supports tooling smoke checks. Counts
cases are separate from timing rows. Idle summary rows use CPU ns/second.
Latency percentiles are in JSONL, including sample count (small samples cannot
reliably estimate extreme tails). A same-binary comparison checks noise; do not
assert a universal speedup threshold on a shared machine.

Optional perf collects cycles, instructions, cache/branch misses, context
switches, migrations and faults for a long throughput run. Its scope is the
whole process, including startup/warm-up/output, unlike the internal measurement
window. Keep raw CSV and do not call whole-process counters exact measured-item
counts. If perf events are unavailable, explicitly report that limitation.

CPU placement, throughput, sparse wakeups, many quiet nodes and active idle work
must be compared independently. Larger maxBatch can trade other strands' tail
latency for throughput; no default scheduling-policy change is implied by a
single benchmark.


## Independent compilation

Use `--split` for final comparisons. In addition to the two general smoke-test
binaries, build `executor_bench_<engine>` and `executor_bench_<engine>_counts`,
where engine is direct, inline, inline_erased, dedicated, pool, moody or legacy. These
specialized targets set EXECUTOR_BENCH_ENGINE and are not built by default;
request their `test/executor_bench/...` targets explicitly with `ninja -j2`.
They instantiate only the selected backend, reducing unrelated template changes'
influence on callback inlining and function layout. The runner requires both
versions' split files and records the actual selected binary's SHA256.

The measured producer loop caches scenario decisions and avoids runtime string
comparisons; the common single-node case also avoids a per-message modulo.
The direct control still includes payload creation, dispatch and correctness
bookkeeping. It is an observer-cost reference, not a guarantee that subtracting
two asynchronous measurements isolates pure executor cost.

The companion simple_graph_bench now accepts an optional variant name, waits
for exact expected counts and measures CPU to completion, rather than through
a quiet window. Its former "src threaded" initializer selected the action
flag by mistake; this is corrected and runtime action-mode checks were added.
Do not compare that historical row by label without accounting for this fix.

## End-to-end pipelines

Build the manual target `test/executor_bench/executor_latency_bench` with
`ninja -j2`. This is a separate single-producer linear pipeline. Each item keeps
its original timestamp across every executor hop; only the last host measures
latency. Every stage checks exact count and FIFO. `--hops 1|4|8` changes thread
boundaries; pool stages share workers. Inline uses the identical topology and
its typed submission path.

```sh
taskset -c 20-27 builddir_release/test/executor_bench/executor_latency_bench \
  --executor dedicated --hops 4 --messages 20000 --spin-us 200 \
  --producer-cpu 20 --consumer-cpu 24
python3 test/executor_bench/run_latency.py \
  --baseline /tmp/executor-latency-final-baseline \
  --candidate builddir_release/test/executor_bench/executor_latency_bench \
  --output /tmp/executor-latency-final --repeats 5
```

`--spin-us` explicitly overrides both executor/pool active-wait policy in this
tool; zero restores blocking. Production defaults are separate from this tool's
zero default. Consumer stage i uses consumer-cpu + i%4, and pool worker i uses
consumer-cpu + i%4; select allowed physical CPUs, ideally separate from producer.
Use `--consumer-cpus 24,26,28,30,32,34,36,38` for an explicit list instead of
the default four-CPU cycle. Validate physical core/NUMA placement on your machine;
the runner accepts the same option and an enclosing `--cpus` mask.
The runner records actual binary hashes and alternates randomized process order.
Build the **same final source** with original headers and
`-DEXECUTOR_LATENCY_BASELINE=1` before production changes; this build accepts only
spin-us=0. Do not compare different measurement loops.

Default mode `closed` waits for terminal acknowledgement, then submits the next
item. Optional interval-us sleeps after each completion, isolating cold wakeups
without queue buildup. The producer uses pause while awaiting acknowledgement;
its CPU is reported separately. `--mode periodic --interval-us 50` uses absolute
scheduled arrivals, does not await each completion, and can build a queue.
The JSON reports actual submit-to-terminal p50/p99/p999, scheduled-arrival p99,
and producer-lateness p99 so missed scheduling deadlines are visible. Increasing
arrival rate beyond service capacity increases latency; this is not a wakeup
measurement. Periodic mode with interval zero submits a saturation burst.

Items carry three uint64 fields without business allocations. Sample storage is
preallocated and final sorting occurs outside measurement. Clock reads and the
pipeline forwarding call are included equally in Inline. The pipeline model
isolates executor boundaries; it does not replace RealTimeApp graph integration
or the concurrent multi-producer correctness suite. Report sample count and CPU
alongside latency; 5000 samples do not support a stable extreme-tail guarantee.

The runner optionally accepts `--idle-baseline` and `--idle-candidate`, pointing
to the corresponding generic executor_bench binaries. It then appends a separate
128-node idle comparison (300 ms, with/without idle callbacks), retaining
idle.jsonl and idle-summary.md. These cases check that active waiting stops;
they do not contribute to latency percentiles.

## Historical develop baseline

Archived measurements retain their historical `legacy` labels. The old Legacy
class, header, benchmark engine and `--include-legacy` runner flag have been
removed. Use `--executor dedicated` for the current mutex/list default.
`run_latency.py --engines dedicated,enhanced` compares the current backends.

## Enhanced dedicated executor

`--executor dedicated` selects infra's default mutex/list executor. `--executor enhanced` selects
`basic::EnhancedDedicatedThreadedExecutor` (MPSC plus bounded Boost.Lockfree
node reuse). Split binaries are `executor_bench_enhanced` and `_counts`.
`ExecutorLatencyBench` accepts `--executor enhanced` for multi-hop comparisons.
Historical result files retain the original meanings of their engine names.

`run_enhanced_profile.py --baseline <unpooled-enhanced> --candidate <enhanced>
--baseline-counts <unpooled-counts> --candidate-counts <enhanced-counts>
--output <directory>` runs five alternating comparisons on CPUs 20–27.
Build both versions with the same current benchmark source, flags and engine=7;
only substitute the Enhanced header for the baseline. The baseline is the old
MPSC implementation renamed into basic without node reuse. Do not compare the
new default against the old MPSC and attribute the result solely to node reuse.

Use `python3 prepare_unpooled_enhanced.py --infra-repo <infra-worktree>
--revision ff08700 --output /tmp/mpsc-baseline` to reproduce the historical
baseline include override. Add `-I/tmp/mpsc-baseline` before the current source
include roots when compiling the baseline with `-DEXECUTOR_BENCH_ENGINE=7`.
The candidate uses the current basic header. For counts add
`-DEXECUTOR_BENCH_COUNTS=1 -Wl,--wrap=malloc -Wl,--wrap=free`.

## Low-latency placement and spin budget

The Enhanced executor remains asynchronous. Compare it with Inline under the
same payload and pacing; a lower latency achieved by continuously spinning has
a CPU cost. `ThreadedExecOptions::spinWait` is the direct executor option; the
benchmark exposes it as `--spin-us`. No default budget or affinity is changed. With idle work enabled, the active
wait is additionally capped by the executor idle period.

For this host, sysfs reports CPU20/48 as SMT siblings. Reproduce the optional
one-hop, 1ms-interval configuration with:

```sh
taskset -c 20,48 builddir_release/test/executor_bench/executor_latency_bench \
  --executor enhanced --producer-cpu 20 --consumer-cpus 48 --hops 1 \
  --spin-us 2000 --interval-us 1000 --messages 500 --warmup 16
```

Compare `--executor inline` and spin budgets 0/200/2000; inspect process_cpu_ns
relative to wall_ns as well as latency. Determine sibling CPUs from the actual
host topology. SMT can reduce cache-transfer latency but shares execution
resources with real callback work. These small-message results do not establish
business-workload throughput or extreme-tail guarantees. Historical result
files and the task record keep placement gains separate from code gains.

### 多生产者噪声复核

`run_contention_profile.py` 每轮随机运行两个相同基线和一个候选，默认30轮；保存命令、二进制SHA256和原始结果，按轮配对bootstrap计算平均CPU/item变化的95%区间。两个基线标签用于检查机器噪声，不能只比较少量样本的中位数。使用未启用counts的二进制，并按本机拓扑调整CPU列表：

```sh
python3 test/executor_bench/run_contention_profile.py \
  --baseline /tmp/baseline/executor_bench_enhanced \
  --candidate builddir_release/test/executor_bench/executor_bench_enhanced \
  --output /tmp/contention-profile
python3 test/executor_bench/run_contention_profile.py \
  --summarize-only /tmp/contention-profile/results.jsonl \
  --output /tmp/contention-summary
```

复算使用与测量相同的 `--seed`（默认20261108）。区间仅描述当前负载和机器的测量，不代表所有部署环境。
