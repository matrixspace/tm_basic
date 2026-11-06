# Executor profiling

Direct Executor/Host calls, without AppRunner, clocks in Environment, TimeChecker,
logging or business allocation. Existing threaded-handler tests remain the
correctness suite; simple_graph_bench remains the graph integration benchmark.

Build release targets `test/executor_bench/executor_bench` and
`test/executor_bench/executor_bench_counts`, after sourcing the workspace build
environment. Use `ninja -j2`; do not build the entire examples tree for profiling.

## Cases and metrics

- `--executor direct|inline|inline-erased|dedicated|pool|moody`.
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
  --cpus 20-27 --producer-cpus 20,21,22,23 --consumer-cpus 24,25,26,27 --repeats 5 --perf --output /tmp/executor-comparison
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
