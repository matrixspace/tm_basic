# 修复条件变量无谓词 1ms 轮询等待

**任务开始时间：** 2026-11-17
**任务结束时间：** 2026-11-17
**状态：** 完成
**分支：** `feature/fix-cv-1ms-poll`（tm_infra `e18aa91`，tm_basic `5b72e3e`，tm_transport `90a5928`）

## 背景

tm_kit 中若干自建线程节点（未走统一执行器的本地 `std::thread` 循环）在条件变量上使用
`cond_.wait_for(lock, std::chrono::milliseconds(1))` 且不带谓词：即使生产者已经 `notify_one()`，
线程仍以 1ms 超时兜底，空闲时每 1ms 空转一次（浪费 CPU / 定时器中断），停止或数据到达最多多等 1ms。
部分停止路径（析构 / `stopThread`）甚至没有 `notify`，完全依赖 1ms 超时来发现 `running_ == false`。

这是 20261105-realtime-threaded-executor 任务"去掉 1ms 白等"的漏网之鱼——它们是不走统一执行器、
直接 `std::thread` + `detach()` 的本地循环。

## 方案（含被放弃的方案和原因）

**采用**：把无谓词的 `wait_for(lock, 1ms)` 改为带谓词的阻塞 `cond_.wait(lock, pred)`，
谓词为 `!running_ || <队列非空>`；同时给所有设置 `running_ = false` 的停止路径补 `cond_.notify_one()`。
这样空闲时线程完全阻塞在条件变量上，不空转；数据到达 / 停止被 `notify` 立即唤醒。

**放弃**：沿用 legacy 执行器的 `wait_for(lock, 1ms, pred)`（保留 1ms idle 周期）。
原因：legacy 执行器有 idle tick 语义需要周期唤醒；本次处理的节点没有 idle 回调，
保留 1ms 会继续空转，无法消除性能问题。

**明确不修改**（技术上有意，非本 bug）：
- `ChainWriter.hpp` `ThreadedHandler` 的 `wait_for(1ms, pred)`：1ms 驱动 `idleWork()` 回调，是有意的 idle 周期。
- `CommonFlowUtils.hpp` `actionWithBufferedState` 的 `wait_for(100ms)`：无输入时周期性调用 `outputProducer` 产生输出的节拍。
- `RealTimeThreadedExecutor.hpp` / `RealTimeSharedPoolExecutor.hpp` 的 `wait_for(idlePeriod/idleTick, pred)`：可配置 + 有谓词的 idle tick。
- `std::future::wait_for(timeout)` 与 transport 层 `sleep_for(10ms)` 网络轮询：语义不同。

## 代码改动（按文件列出）

- tm_basic
  - `tm_kit/basic/CommonFlowUtils.hpp`：`BunchAction::runThread` 的 `wait_for(1ms)` → `wait(pred)`；`stopThread` 补 `notify_one`。
  - `tm_kit/basic/transaction/v2/TransactionFacility.hpp`：`runTransactionThread` 的 `wait_for(1ms)` → `wait(pred)`；析构补 `notify_one`。
  - `tm_kit/basic/transaction/v2/GeneralSubscriber.hpp`：`runWorkThread` 的 `wait_for(1ms)` → `wait(pred)`；析构补 `notify_one`。
  - `tm_kit/basic/simple_shared_chain/ChainWriter.hpp`：`waitForStart` 的 `wait_for(1ms)` → `wait(pred)`。
- tm_infra
  - `tm_kit/infra/RealTimeApp.hpp`：`constTriggerImporter` / `triggerImporter` / `BunchedImporter` 的 `wait_for(1ms)` → `wait(pred)`。
- tm_transport
  - `tm_kit/transport/grpc_interop/GrpcClientFacility.hpp`：`waitForStart` 的 `wait_for(1ms)` → `wait(pred)`。

## 测试与验证（命令、构建类型、结果）

构建环境：`source build-common/build-env-setting.sh`，gcc 15.2.1，release 构建，`ninja -j4`。

- tm_infra：`builddir_release` 全量 17/17 目标构建 + install 成功。
- tm_basic：`common_flow_utils_cpu_bind_test`（覆盖 `bunchWithProcessing`/BunchAction）、
  `simple_graph_bench`（覆盖 `triggerImporter`）、`chain_writer_loop_test`（覆盖 `ChainWriter::waitForStart`）
  三个目标编译链接成功，仅有既有 unused-parameter 警告。
- tm_transport：`threaded_handler_transport_test`（覆盖 `triggerImporter` 在 transport 上下文）编译链接成功。
- tm_examples（用 install 的改后头文件重编译）：
  `transaction_server`（覆盖 `TransactionFacility`/`TransactionProcessor`）、
  `grpc_interop_test/CppClient/cpp_client`（覆盖 `GrpcClientFacility`）、
  `db_subscription/db_subscription_server`（覆盖 `GeneralSubscriber`）编译链接成功。

未单独编译验证（与已验证的 `triggerImporter` 同构，且更简单）：
- `constTriggerImporter`（仅在 GUI 客户端使用，测试环境无 FLTK/WX 依赖）
- `bunchedImporter`（代码库中无使用点）

## 兼容性与注意事项

- 阻塞 `wait` 依赖停止路径必然 `notify`，否则析构 / join 会死锁；本次已逐一核对并补齐
  （BunchAction 的 `stopThread`、TransactionProcessor/GeneralSubscriber 的析构原来均缺 notify）。
- detach 线程的析构竞态（对象析构时线程仍可能访问成员）是原有设计，本次通过 `notify` 让线程更快退出，不引入新竞态。
- 空闲时不再每 1ms 空转；数据到达 / 停止由 `notify` 立即唤醒，语义与原来等价（甚至更优）。

## 相关文档

- 主仓库 tm_basic；tm_infra / tm_transport 的 `doc/INDEX.md` 各加一条链接指向本记录。
- 关联：tm_infra `20261105-realtime-threaded-executor`（统一执行器已去掉 1ms 白等，本次为漏网自建循环）。
