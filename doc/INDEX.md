# tm_basic 开发任务索引

| 任务 | 时间 | 概述 |
|---|---|---|
| [20261029-tsc-clock-monotonic](20261029-tsc-clock-monotonic/README.md) | 2026-10-29 | TscClock 保证读取时间非递减：重同步不再因 system_clock 回拨而后退，退化路径增加高水位钳制 |
| [→ tm_infra/20261105-realtime-threaded-executor](../../tm_infra/doc/20261105-realtime-threaded-executor/README.md) | 2026-11-05 起 | （进行中：第二阶段执行器抽象重构；主记录在 tm_infra）统一 `MoodycamelThreadedExecutor<Item>`，ChainWriter 改用本地循环并修掉 1ms 白等；新增 `test/chain_writer_loop_test`，完善图框架基准并新增独立 executor profiling（计时 / 分配移动计数分离、角色绑核、独立后端编译、多跳端到端延迟）；[端到端延迟优化](../../tm_infra/doc/20261105-realtime-threaded-executor/executor-latency-optimization.md)；[优化记录](../../tm_infra/doc/20261105-realtime-threaded-executor/executor-overhead-optimization.md)；2026-11-07 增加 [Legacy 执行器基线、等待修复及复测](../../tm_infra/doc/20261105-realtime-threaded-executor/legacy-executor-baseline.md)；2026-11-07 [默认执行器回迁与 Enhanced MPSC 节点复用](../../tm_infra/doc/20261105-realtime-threaded-executor/mpsc-node-recycling.md)，infra 默认 mutex/list，MPSC 迁至 basic，以 Boost 缓存减少分配；按用户要求删除 Legacy 兼容入口；继续评估缓存容量并优化回收节点批量读取；2026-11-08 完成延迟关键路径、Inline/SMT、自旋预算及90次双基线对照 |

- 2026-11-09：[执行器实现分层](../../tm_infra/doc/20261105-realtime-threaded-executor/executor-layering.md)：SharedPool及其队列迁到basic，infra仅保留Inline/Dedicated默认实现。

- 2026-11-09：[Enhanced取消全局FIFO后的优化](../../tm_infra/doc/20261105-realtime-threaded-executor/unordered-enhanced.md)：采用moodycamel分块队列与分片发布计数，以单生产者部分延迟代价换取多生产者吞吐。

- 2026-11-09：[moodycamel执行器合并](../../tm_infra/doc/20261105-realtime-threaded-executor/moodycamel-unification.md)：只保留MoodycamelThreadedExecutor，以可用性探测控制相关目标编译。

- 2026-11-11：[Threaded 节点独立配置 spinWait 与绑核](../../tm_infra/doc/20261111-threaded-node-spin-wait-affinity/README.md)：MoodycamelThreadedExecutor 支持节点级 spinWait 与绑核。

- 2026-11-11：[节点线程自动绑核（CpuAutoBind）](../../tm_infra/doc/20261111-thread-cpu-autobind/README.md)：新增可选的 HwlocThreadCpuAllocator 与 meson 选项 hwloc_cpu_allocator。
