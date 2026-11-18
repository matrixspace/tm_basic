# 自带线程节点去 detach、实现优雅停止

**任务开始时间：** 2026-11-18
**任务结束时间：** 2026-11-18
**状态：** 完成
**分支：** `feature/graceful-thread-stop`（tm_infra `df0f645` / tm_basic `01519b9`）

## 背景

`tm_basic/tm_kit/basic/CommonFlowUtils.hpp` 的 `bunch()`/`bunchWithProcessing()` 内部 `BunchAction`（约 L191）用 `std::thread` 起了一个 worker 线程，构造时 `th_.detach()`，析构只 `stopThread()`（置 `running_=false` + `notify`）而**不 join**。线程在对象析构后仍可能访问 `this`（`processing_` 队列、`this->publish`），构成 use-after-free，且无法优雅停止。

扫描 `tm_infra / tm_basic / tm_transport` 后发现，这并非个例，而是一类**同构反模式**：自带线程的节点在构造/`start()` 里 `th_.detach()`，同时又在析构里写 `if (th_.joinable()) th_.join()`——`detach` 之后 `joinable()` 恒为 false，这个 join 是死代码。目标是：**默认不 detach，析构真正 join，实现优雅停止**；只有在 join 化会引入侵入性改动（改队列数据类型、塞哨兵停止标志、或对正常数据路径有较大影响）时才保留 detach。

## 方案

### 统一修复模式

与 `DedicatedThreadExecutor` / `MoodycamelThreadedExecutor`、以及 transport 各 `*Component.cpp` 里**已正确实现**的写法保持一致：

```cpp
// start()/构造函数：running_ 在建线程之前置 true，去掉 detach
running_ = true;
th_ = std::thread(&X::run, this);

// 析构：置 false + notify（有 cond 时），然后 join（自 join 不可能时退回 detach）
running_ = false;
cond_.notify_one();
if (th_.joinable()) {
    if (th_.get_id() == std::this_thread::get_id()) th_.detach();
    else th_.join();
}
```

要点：
- `running_` 必须**在建线程之前**置 true（消除"析构与子线程首行置位"的 race）。
- 析构里 join 不能再用外层 `if (running_)` 兜底——它会漏 join，正确判据是 `th_.joinable()`。
- 自 join 不可能（析构从本线程回调内触发），此时退回 detach，线程在回调返回后自行退出（与执行器一致）。

### 可中断 sleep（用于文件回放 importer 6/7/8）

这些 importer 的 `run()` 用 `env_->sleepFor(虚拟时长)` 睡到下一条记录的时间戳。`Clock::sleepFor` 内部是 `std::this_thread::sleep_for(actualDuration(虚拟时长))`（`actualDuration` 把虚拟时长按 `1/speed` 换算成真实时长）。要让 stop/析构能立即唤醒睡眠，把 `sleepFor` 换成可中断等待：

- 为 `LocalI` 新增 `std::mutex mutex_` + `std::condition_variable cond_`。
- 睡眠处改为：
  ```cpp
  {
      std::unique_lock<std::mutex> lock(mutex_);
      cond_.wait_for(lock, env_->actualDuration(X), [this]{ return !running_; });
  }
  if (!running_) break;  // stop 在睡眠中到达：不再 publish
  ```
- `control("stop")` 与析构改为：`running_ = false; cond_.notify_all();` 再 join。

注意保持 `speed` 缩放语义：等待时长用 `env_->actualDuration(X)`（与 `Clock::sleepFor` 内部同款换算），不等价于直接 `wait_for(X)`。若 `Env` 未暴露 `actualDuration`，则退化为"小切片 `sleepFor` + 每片检查 `running_`"的等价实现（`actualDuration` 线性，切片求和语义不变）。

### 可改动点（共 14 处）

#### tm_basic

1. **`CommonFlowUtils.hpp` `BunchAction`（约 L191–299）——主目标**
   - 现状：构造 L271–276 `running_=true; th_=std::thread(&runThread); handle_=native_handle(); th_.detach();`；析构 L277–279 只有 `stopThread()`，完全不 join。
   - 问题：最严重一处，use-after-free + 无法优雅停止。
   - 改动：构造去掉 detach；析构 `stopThread()` 后 join（self-join guard）。`handle_` 注释由 "cache before detach" 改为 "cached while joinable"。
   - 影响：数据路径零变化；`cond_.wait` 被 notify 立即唤醒，join 即时；`threadHandle()`/`configureThread` 不变。

2. **`CommonFlowUtils.hpp` `Delayer`（约 L1561–1656）**
   - 现状：构造 `running_(true)` + detach；析构有 join 但是死代码。
   - 改动：去 detach；析构 join（self-join guard）。
   - 影响：`run()` 用 `env_->sleepFor(...)`，不可被 running_ 中断，join 最长阻塞到当次 sleep 结束（≤ `duration_`）。短延迟无感；长延迟析构会等当次 sleep。这是既有意图（原死代码就想 join），本次不引入可中断 sleep（可选后续项）。

3. **`CommonFlowUtils.hpp` `actionWithBufferedState` 的 `LocalAction`（约 L2310–2471）**
   - 现状：构造 detach；析构死 join。
   - 改动：去 detach；析构 join（self-join guard）。`run()` 用 `cond_.wait_for(100ms)` 检查 running_，notify 后即退出。
   - 影响：即时 join，无数据路径影响。

4. **`simple_shared_chain/ChainWriter.hpp` `chain_writer_detail::ThreadedHandler`（约 L26–130）**
   - 现状：`startThread()` detach；析构死 join。
   - 改动：去 detach；析构 join + self-join guard。`run()` 用 `cond_.wait_for(1ms)` 检查 running_。
   - 影响：join 延迟 ≤1ms。

5. **`simple_shared_chain/ChainWriter.hpp` `chain_writer_detail::BusyLoopThreadedHandler`（约 L130–210）**
   - 现状：`startThread()` detach；析构死 join。
   - 改动：去 detach；析构 join + self-join guard。忙等循环每轮查 running_。
   - 影响：join 即时。

6. **`ByteDataWithTopicRecordFileImporterExporter.hpp` `createImporter` 的 `LocalI`（约 L25–112）**
   - 现状：`start()` detach；`control("stop")` `running_=false; join`（死 join）；**无自定义析构**。
   - 问题：use-after-free；stop 本意优雅停止却从不生效。
   - 改动：去 detach；`control("stop")` 的 join 真正生效 + self-join guard；**新增析构兜底 join**（否则线程 joinable 时对象销毁会 `std::terminate`）；把 `env_->sleepFor(d->timePoint - t)` 换成可中断 sleep（见"可中断 sleep"小节）。
   - 影响：stop/析构可立即唤醒睡眠中的线程，join 不再阻塞一个记录间隔；数据时间戳驱动、`speed` 缩放语义保持不变。

7. **`GenericIOStreamImporterExporter.hpp` `createImporter(GenericIStreamImporterSpec)` 的 `LocalI`（约 L147–220）**

8. **`GenericIOStreamImporterExporter.hpp` `createImporter(stream, reader)` 的 `LocalI`（约 L222–308）**
   - 现状/问题/改动/影响：与 6 同构（`start()` detach + `control("stop")` 死 join + 无析构，`run()` 有 `sleepFor`）。同样去 detach、join 生效、新增析构兜底，并把 `env_->sleepFor(...)` 换成可中断 sleep。
   - 说明：`createExporter` 的 `separateThread` 分支走 executor（`ExecHandler`），无裸 detach，不在改动范围。

9. **`transaction/v2/GeneralSubscriber.hpp` `GeneralSubscriber`（约 L443–460）**
   - 现状：析构 `if(running_){running_=false; notify; join}`；`start()` `running_=true; std::thread; detach()`（死 join）。
   - 改动：去 detach；析构 join + self-join guard（`if constexpr (M::PossiblyMultiThreaded)` 保持）。`run()` 用 `inputQueueCond_.wait` 检查 running_。
   - 影响：即时 join；`running_` 在 start() 已先置 true，无 race。

10. **`transaction/v2/TransactionFacility.hpp` `TransactionProcessor`（约 L385–401）**
    - 现状/改动/影响：与 9 同构；去 detach + 析构 join + self-join guard。

#### tm_infra

11. **`RealTimeApp.hpp` `UniformSimpleImporter`（约 L2246–2292）**
    - 现状：`start()` detach；析构 `if(running_){running_=false; join}` 死代码。
    - 改动：去 detach；析构改无条件 join-if-joinable + self-join guard（去掉外层 `if(running_)`）。
    - 影响：`running_` 已建线程前设 true（L2282），无需移动；即时 join。

12. **`RealTimeApp.hpp` `constTriggerImporter` 的 `LocalI`（约 L2314–2373）**

13. **`RealTimeApp.hpp` `triggerImporter` 的 `LocalI`（约 L2411–2470）**
    - 现状：`run()` 顶部**在子线程里** `running_=true`；`start()` 建线程后 detach；析构 `if(running_){...join}` 死代码。
    - 额外问题：`running_` 在 run() 里才置 true，若 start() 建线程后、run() 置 true 前析构，析构 `if(running_)` 为 false 会漏 join（当前靠 detach 掩盖）；且 run() 可能在析构置 false 后把 running_ 写回 true，导致 join 卡死。
    - 改动：把 `running_=true` 从 `run()` 移到 `start()` **建线程之前**，`run()` 顶部删除该行；去 detach；析构改无条件 join + self-join guard。
    - 影响：修复上述 race；`trigger()` 之前"start 后、run 置 true 前触发被丢"的小窗口顺带消失（正向修正）；数据路径不变。

14. **`RealTimeApp.hpp` `BunchedImporter`（约 L2516–2610）**
    - 现状：`start()` `running_=true; std::thread; detach()`；析构 `stopThread(); join`（死 join）。
    - 改动：去 detach；析构 join + self-join guard（`stopThread()` 已含 false+notify）。
    - 影响：即时 join；run() 在 final 时自设 running_=false，析构 join 已结束的线程无碍。

### 保持 detached 的点（有充分理由，不动）

- **`RealTimeApp.hpp` `SimpleImporter`（约 L2201）**：线程跑一次性 `f_(pub)`（用户导入函数，可能长期阻塞，无 running_ 循环）。优雅停止需给 `f_` 注入停止契约（改上层 API）或哨兵，属对上层 API 有影响，保留。
- **`RealTimeSharedPoolExecutor`（tm_basic）**：共享线程池，文档明确 detached 生命周期；池线程服务多个节点，不能随单节点析构 join。保留。
- **传输层各 `*ImporterExporter.hpp` 的 `control()`**（zeromq/nng/redis/nats/rabbitmq/websocket/multicast/singlecast/aeron_ipc/shm_cursor/shm_seqlock/shared_memory_broadcast）：一次性 fire-and-forget 控制线程（`removeSubscriptionClient` 等可能阻塞网络操作）；真正工作线程在底层 `*Component.cpp`，其析构已正确 join。保留。
- **一次性任务线程**：`simpleTaskSpawningFacility`（CommonFlowUtils）、`consumeUntilLastFuture`（RealTimeApp）、`sendStopToAllNodes`（WithTimeData）、Redis/NATS/RabbitMQ/WebSocket `OnOrderFacility` 的 `.detach()`——短生命周期任务/回调线程，无持久 running_ 循环；其中 `sendStopToAllNodes` 是停机入口本身，join 化需重设计停机握手，影响面大。保留。
- **`ClockComponent`（tm_basic src）**：进程级常驻时钟线程，无 per-node 停止语义。保留。

### 已定决策

- **A**：6/7/8 的 `sleepFor` 改为可中断 sleep（见上），stop/析构不再阻塞到当次 sleep 结束。
- **B**：14 处全部纳入本次开发。

## 代码改动（按文件列出）

- `tm_basic/tm_kit/basic/CommonFlowUtils.hpp`（3 处：BunchAction / Delayer / actionWithBufferedState::LocalAction）
- `tm_basic/tm_kit/basic/simple_shared_chain/ChainWriter.hpp`（2 处：ThreadedHandler / BusyLoopThreadedHandler；并更新 stale 注释）
- `tm_basic/tm_kit/basic/ByteDataWithTopicRecordFileImporterExporter.hpp`（1 处：createImporter 的 LocalI，sleepFor → 可中断 wait_for）
- `tm_basic/tm_kit/basic/GenericIOStreamImporterExporter.hpp`（2 处：两个 createImporter 的 LocalI，sleepFor → 可中断 wait_for）
- `tm_basic/tm_kit/basic/transaction/v2/GeneralSubscriber.hpp`（1 处）
- `tm_basic/tm_kit/basic/transaction/v2/TransactionFacility.hpp`（1 处）
- `tm_infra/tm_kit/infra/RealTimeApp.hpp`（4 处：UniformSimpleImporter / constTriggerImporter / triggerImporter / BunchedImporter）
- `tm_basic/test/common_flow_utils_cpu_bind_test/CommonFlowUtilsCpuBindTest.cpp`（同步更新 stale 注释）

## 测试与验证

构建类型 `debug`，`source build-common/build-env-setting.sh`，`NINJA_JOBS=4`，各 worktree 独立 builddir。

- **tm_infra**（`worktrees/tm_infra-graceful-thread-stop/builddir_debug`）：
  - 编译并运行通过：`trigger_importer_test`、`bunched_importer_test`、`const_trigger_importer_test`、`threaded_handler_test/dedicated_executor_test`、`threaded_handler_test/threaded_node_options_test`（全部 `ALL PASSED`）。
- **tm_basic**（`worktrees/tm_basic-graceful-thread-stop/builddir_debug`）：
  - 编译并运行通过：`common_flow_utils_cpu_bind_test`、`chain_writer_loop_test`、`shared_pool_executor_test/threaded_handler_test`、`moodycamel_executor_test/moodycamel_executor_test`（全部 `ALL PASSED`）；`bunch_throughput_bench` 编译通过（未运行基准）。
- **tm_examples**（复用 `tm_examples/builddir_debug`，链接已安装的 worktree tm_infra/tm_basic 头文件）：
  - 编译通过：`serialization_test/generic_io_stream_importer_exporter_test`（验证 GenericIOStream 可中断 sleep）、`transport_utils/tm_historical_republisher`（验证 ByteData 可中断 sleep）、`transaction_redundancy_test/transaction_server`（验证 transaction v2 GeneralSubscriber/TransactionProcessor）。
  - 运行通过：`generic_io_stream_importer_exporter_test`（`all checks passed`）。

关键验证点：析构/stop 路径从"立即返回（线程继续跑）"变为"阻塞 join 等线程退出"，`actualDuration` 保持 `speed` 缩放语义（与 `TimestampedCollectionImporter` 既有用法一致）。

## 兼容性与注意事项

- 行为兼容：正常数据路径零变化；变化仅在析构/stop 时从"立即返回（线程继续跑）"变为"阻塞等线程退出"。
- `delayer` 的 `run()` 仍用 `env_->sleepFor(...)`（不可被 running_ 中断），析构 join 最长阻塞到当次 sleep 结束（≤ `duration_`）；本次未改可中断 sleep（仅文件回放 importer 6/7/8 按决策改了）。
- 需同步更新 `tm_kit_reference/tm_infra/10-realtime-executors.md`：把"仍 detached 的节点"清单中已改的节点移出，并更新"`detach()` 之前缓存 native_handle"那处说明。
- 仍有少量测试 harness 注释写着"detached … outlive main"（tm_infra 的 trigger/bunched/const_trigger importer 测试与 bench、tm_basic 的 bunch_throughput_bench 等）：这些是"为何泄漏 graph/env"的历史注释，现已不再准确但泄漏写法仍无害；`common_flow_utils_cpu_bind_test` 的注释已同步更新，其余可后续清理。

## 相关文档

- 关联任务：[20261117-fix-cv-1ms-poll](../20261117-fix-cv-1ms-poll/README.md)（此前把这些节点的无谓词 `wait_for` 轮询改为带谓词 `wait` 并补齐停止路径 `notify_one`，本次在其基础上把 detach 收掉、join 生效）
- tm_kit_reference 章节：`tm_infra/10-realtime-executors.md`（执行器析构 join 契约、detached 节点清单）
