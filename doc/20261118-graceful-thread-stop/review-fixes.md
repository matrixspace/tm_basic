# 线程停止与环境兼容性评审修复

**任务开始时间：** 2026-11-18  
**任务结束时间：** 2026-11-18  
**状态：** 完成  
**分支：** `fix/20261118-review-basic`（代码提交 `a169274`）

评审发现队列等待可能漏掉停止通知，ChainWriter 初始化抛异常后可能无法退出，回放增加了 clock 接口要求且控制路径可能持锁等待线程，Delayer 可能睡到远期时间戳。绑核测试还需要隔离租约并禁止空闲输出。

回放和 Delayer 共用可中断时间等待。没有 `actualDuration()` 的环境通过短片 `sleepFor()` 保持 clock 语义。回放的控制命令只发出停止请求，析构负责等待退出。

ChainWriter 完成初始扫描后再启动 worker，保留启动屏障。绑核测试的两个模式各用一个子进程。长间隔的 IStreamPollingLinesImporter 也接入 infra 新增的停止上下文。

GCC 15 debug 首轮验证通过 `chain_writer_loop_test`、`common_flow_utils_cpu_bind_test`。后者额外在 `taskset -c 2,30`（一个物理核）下复测，8 项绑核断言全部通过。

## 改动与验证

- `BunchAction`、`GeneralSubscriber`、`TransactionProcessor` 在各自队列锁内清除运行标志，再发出通知。
- 回放覆盖 GenericIOStream 的两个重载和 ByteData 记录回放。有 `actualDuration()` 时用条件变量和实际时长，无该方法时使用 10ms 虚拟时长切片。控制线程发出停止请求，析构等待 worker。停止请求不能打断已经阻塞的任意流读取。
- Delayer 的远期时间等待用同一兼容路径，退出时丢弃尚未发布的消息。IStreamPollingLinesImporter 的轮询等待也可取消。
- `realtime_shutdown_test` 验证三种回放、仅实现 `sleepFor()` 的环境、有时间缩放方法的环境、远期 Delayer、回放回调进行中的 `controlAll` 和长间隔流轮询退出。ChainWriter 测试覆盖两种 worker 的初始化异常。

Debug 增量构建上述三个目标，使用 `meson test -C builddir_debug --no-rebuild --print-errorlogs realtime_shutdown_test chain_writer_loop_test common_flow_utils_cpu_bind_test`，3/3 通过。

相关记录：[原停止线程任务](README.md)、[绑核测试](../20261117-common-flow-utils-cpu-bind/README.md)、[infra 评审修复](../../../tm_infra/doc/20261118-realtime-queue-deque-optimization/review-fixes.md)。
