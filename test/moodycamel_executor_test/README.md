# Moodycamel executor

Include `tm_kit/basic/MoodycamelThreadedExecutor.hpp`. When concurrentqueue headers
are available the header defines `TM_KIT_BASIC_HAS_MOODYCAMEL_THREADED_EXECUTOR`.
Opt in using `basic::MoodycamelThreadedExecutorComponent` in the Environment.
`MoodycamelThreadedExecutor<Item, Config=infra::DedicatedThreadExecutorConfig>`
uses one dedicated detached worker and serial callbacks. Config controls idlePeriod
and spinBeforeSleep; ThreadedExecOptions controls spinWait and trackPendingCount.

This combines the former Enhanced implementation's 256-item blocks, ConsumerToken,
in-place dequeue without an additional Item move, eight publication shards and
bounded spinning with the existing optional Moodycamel API. It replaces the old
BlockingConcurrentQueue/optional wake-token implementation. Only per-producer FIFO
is guaranteed. Explicit CheckTime=true and version checks can reject reordered
inputs; the infra default remains DedicatedThreadExecutor.

Meson option `moodycamel_executor=auto` probes either supported header layout.
`enabled` requires headers; `disabled` skips the tests and dedicated moody benchmarks
and defines TM_KIT_BASIC_DISABLE_MOODYCAMEL_THREADED_EXECUTOR during this build.
Without the library, the header remains includable but declares no executor or
component. The header is installed regardless, allowing downstream header detection.
Enhanced header/type/component/benchmark aliases have been removed.

```sh
ninja -j2 -C builddir_release \
  test/moodycamel_executor_test/moodycamel_executor_test \
  test/moodycamel_executor_test/moodycamel_queue_test
meson test -C builddir_release --no-rebuild moodycamel_executor_test moodycamel_queue_test
```

Queue tests cover move-only non-default/non-assignable payloads, throwing moves and
callbacks, exact counts/per-producer order under 16 producers, repeated block reuse,
producer exit and queue address reuse. Executor tests cover idle/stop/wake races and
paced publication with no idle token; the handler suite covers graph variants,
serial callbacks and explicit time checking. Detached hosts are intentionally
retained, so ASan uses detect_leaks=0. GCC TSan cannot model some library fences.

[Task record](../../../tm_infra/doc/20261105-realtime-threaded-executor/moodycamel-unification.md).
