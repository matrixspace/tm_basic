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
within each producer, repeated cache overflow/reuse, move-only non-default
payloads, throwing construction/consumption, destruction with queued data,
startup, tracked pending counts, dynamic idle work and callback-safe stop.
The handler target reuses the existing executor integration suite with Enhanced
selected: graph pure/multi/variant actions, FireOnceOnly, dynamic idle,
CheckTime, serial callbacks, concurrent input and stop. It also runs the new
infra default as a comparison. Detached test hosts deliberately outlive callbacks.

Sanitizers can compile the direct test with `-std=c++17 -O1 -g -pthread`,
`-fsanitize=thread` or `-fsanitize=address,undefined`; supply the infra/basic
source include roots and installed Boost include directory. Use
`ASAN_OPTIONS=detect_leaks=0` for the intentionally retained detached hosts.
Timing/allocation comparisons belong in `../executor_bench` and must not run
concurrently with builds or sanitizer stress tests.

[Task record](../../../tm_infra/doc/20261105-realtime-threaded-executor/mpsc-node-recycling.md)

Node recycling uses eight shards, each with a 32-pointer Boost SPSC queue and
an eight-pointer read batch protected by the shard admission flag. The queue
owns both buffers; producer exit needs no cleanup callback. At most 320 empty
nodes can be retained; overflow still goes to delete and misses to new.
