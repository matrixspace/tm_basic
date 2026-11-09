# Enhanced dedicated executor tests

Source public header: `tm_kit/basic/EnhancedDedicatedThreadedExecutor.hpp`.
Opt in by adding `basic::EnhancedDedicatedThreadedExecutorComponent` to the
Environment, or use `infra::RealTimeThreadedExecutorComponent<basic::EnhancedDedicatedThreadedExecutor>`.
The infra default remains mutex/list `DedicatedThreadExecutor`.

After sourcing `build-common/build-env-setting.sh` and configuring this worktree's
own Meson build with its private install prefix:

```sh
ninja -j2 -C builddir_release \
  test/enhanced_dedicated_executor_test/enhanced_dedicated_executor_test \
  test/enhanced_dedicated_executor_test/enhanced_handler_test
meson test -C builddir_release --no-rebuild enhanced_dedicated_executor_test enhanced_handler_test
```

The direct tests cover 16 producers (including shard collisions), exact FIFO
within each producer, repeated block turnover/reuse, move-only non-default
payloads, throwing construction/consumption, destruction with queued data,
startup, tracked pending counts, dynamic idle work and callback-safe stop.
The handler target reuses the existing executor integration suite with Enhanced
selected: graph pure/multi/variant actions, FireOnceOnly, dynamic idle,
CheckTime, serial callbacks, concurrent input and stop. It also runs the new
infra default as a comparison. Detached test hosts deliberately outlive callbacks.

Sanitizers can compile the direct test with `-std=c++17 -O1 -g -pthread`,
`-fsanitize=thread` or `-fsanitize=address,undefined`; supply the infra/basic
source include roots and installed concurrentqueue include directory. Use
`ASAN_OPTIONS=detect_leaks=0` for the intentionally retained detached hosts.
Timing/allocation comparisons belong in `../executor_bench` and must not run
concurrently with builds or sanitizer stress tests.

[Task record](../../../tm_infra/doc/20261105-realtime-threaded-executor/unordered-enhanced.md)

The queue now uses moodycamel ConcurrentQueue reusable 256-item blocks, implicit producer
registration and one ConsumerToken. A custom assignment sink invokes the callback
without an extra Item move. Only each producer's order is preserved; cross-producer
FIFO is not guaranteed. Publication counters use eight cache-line-separated shards;
thread-local state retains an integer, not a queue address. Size estimates are only
active-wait hints; sleep uses publication counts. Producer churn/address reuse is
covered by an additional lifecycle test. Block memory retention depends on peak
backlog and producer registrations; the former 320-node cache bound no longer applies.

The direct suite also races 20000 input messages with 20000 idle wake/state
changes, both with spinWait=0 and 200us, then checks final idle activation and
stop. This covers conditional wake acknowledgement and active queue hints.

The sleep publication test sends 1000 paced items without idle wake tokens at
spinWait=0 and 200us. TSan emits GCC warnings for fences inside concurrentqueue;
no runtime race report was observed, but those fences are not modeled by TSan.
