# 优化 bunch / bunchWithProcessing 的吞吐

**任务开始时间：** 2026-11-17
**任务结束时间：** 2026-11-17
**状态：** 完成
**分支：** `feature/bunch-throughput`（commit 见文末）

## 背景

`CommonFlowUtils.hpp` 的 `bunch<T>()` / `bunchWithProcessing<T,Buncher>()`（RealTimeApp 分支共用
同一个 `BunchAction`）是实时模式下把上游单条 `T` 聚合成 `vector<T>`（或经 `Buncher` 聚合）的自建
线程节点：`handle` 在主调线程把输入塞进 `incoming_`，内部 worker 线程按"处理周期"把 `incoming_`
整体搬到 `processing_` 后批量产出。原实现每处理一个输入项都付出两类固定开销：

1. **`std::list<InnerData<T>>` 的逐项堆分配/释放**——`putData` 每次 `push_back` 一个 list 节点（一次
   malloc），消费线程每次 `pop_front` 释放一个节点（一次 free），且 list 遍历缓存局部性差。
2. **`putData` 无条件 `cond_.notify_one()`**——每收一个输入都触发一次 futex 系统调用，即使 worker
   线程正忙、本就不会等待。

在高频/大批量场景下，这两者使节点吞吐被 per-item 固定开销拖低。目标：不改变对外语义（仍按处理
周期聚合，时间/finalFlag/VersionChecker 行为不变），提升吞吐并用基准量化。

## 方案（含被放弃的方案和原因）

**采用（四处改动，均只影响 `BunchAction` 内部）：**

1. `std::list<InnerData<T>>` → `std::deque<InnerData<T>>`。deque 块式分配（约 512B/块），小 `T` 一块
   可装十几~几十项，省去逐项 malloc/free；对任意 `T`（含 move-only、大对象）都安全。
2. 消费线程 `processing_.splice(...)` → `processing_.swap(incoming_)`。`swap` 时 `processing_` 必为空
   （上一轮已清空），语义等价于 splice，且 O(1)、不分配。
3. 消费 drain 由 `front()/pop_front()` 循环改为 `for (auto &data : processing_) {...} processing_.clear();`。
   这是 list→deque 之后必需的配套：`std::deque::pop_front()` 逐个释放块的簿记开销远高于 `clear()`，
   基准实测 big payload 下 `pop_front` 版吞吐只有 `clear` 版的一半（714→341 ns/item）。
4. `putData` 只在队列空→非空时才 `cond_.notify_one()`（`wasEmpty` 捕获），配合同一谓词
   `cond_.wait(lock, [this]{ return !running_ || !incoming_.empty(); })`，忙时逐项 futex 全部省去，
   锁外 notify 避免 hurry-up-and-wait。已论证无丢失唤醒、无活锁。

**被放弃的方案：**

- `std::vector` 双缓冲：`InnerData<T> = TimedDataWithEnvironment<T,Env,TP>`（`WithTimeData.hpp:446`）含
  `Environment* const` 成员且 move 非 `noexcept`，vector 扩容会退化为**逐元素拷贝**（move-only 的 `T`
  直接编译失败，大 `T` 每次扩容全量拷贝）。基准印证：小 payload 下 vector 最快（456 ns/item），但
  big payload 下已无优势（503 vs deque 320），且其正确性/可编译性依赖 `T` 可廉价拷贝，不适合作为
  泛型 `bunch<T>` 的默认容器。
- 无锁 SPSC 队列替换 mutex+cv：去分配与 notify 后 mutex 已非主要开销，改动大且与 `delayer`/
  `actionWithBufferedState` 等自建线程节点的一致性冲突，风险不成比例。

**明确不改：** SinglePass / TopDown 分支（单线程逐项 `vector::push_back` 已近最优）、
`timeChecker_`/`VersionChecker`/`env->resolveTime()`（正确性相关，属语义）。

## 代码改动（按文件列出）

- `tm_basic/tm_kit/basic/CommonFlowUtils.hpp`（`BunchAction`，`:191–270`）：
  - 成员 `incoming_`/`processing_`：`std::list` → `std::deque`；
  - `runThread`：`splice` → `swap`，drain 循环 `front()/pop_front()` → `range-for + clear()`；
  - `putData`：增加 `wasEmpty`，仅空→非空时 `notify_one`。

## 测试与验证（命令、构建类型、结果）

构建环境：`source build-common/build-env-setting.sh`，gcc 15.2.1，release 构建，`ninja -j4`。
worktree `worktrees/tm_basic-bunch-throughput`（分支 `feature/bunch-throughput`），独立 `builddir_release`，
不 install。

新增基准 `test/bunch_throughput_bench/BunchThroughputBench.cpp`（benchmark，非 test，仿
`simple_graph_bench`）：自包含的 `QueueBunchAction<T, Queue>` 副本，以队列容器为模板参数，在同一
线程 + publish 结构下对比三种容器；另含 `prod` 变体直接跑真实 `bunch`/`bunchWithProcessing` 节点。
输入 `PushSource<T>` → bunch → 计数 sink，sink 校验总项数 == N、顺序无丢失/重复（bad==0，全部通过）。

测量方法：`taskset -c 2,3`（先 `mpstat` 确认空闲核；56 核机器、loadavg<1）、500 万条、10 万条预热、
每变体独立进程、5 次取中位数。共享机干扰明显，单次波动可达 ±50%，故以中位数 + 范围为准。

**容器对比（副本，均用条件 notify，wall ns/item 中位数）：**

| payload | `std::list`（原） | `std::deque`（现） | `std::vector`（弃） |
|---|---|---|---|
| small（8B） | 718 | 607 | 456 |
| big（64B） | 972 | 320 | 503 |

**生产节点 `bunch<T>`（真实代码路径，中位数）：** small 约 650（范围 370–870，噪声大）、big 约 340。
原实现（list + 无条件 notify + splice + front/pop_front）的 8B 基线约 950 ns/item（改动前同一基准测得），
优化后约 500–650，**约 1.5–2×**；big payload 下 deque 相对 list 约 **3×**（320 vs 972）。

**回归：** `test/common_flow_utils_cpu_bind_test`（覆盖 `bunch`/`bunchWithProcessing` 构造、绑核、
detached 线程）构建 + 运行 ALL PASSED。

## 兼容性与注意事项

- 对外语义不变：仍按处理周期聚合，`bunch`/`bunchWithProcessing` 的输入/输出类型、finalFlag、
  `TimeChecker`（版本去重 + 时间序）行为完全一致；`configureThreadedNode` 绑核路径不受影响。
- `std::deque` 与 `std::vector` 对 move-only `T` 的差异：deque 引用稳定、`push_back` 不移动已有元素，
  天然支持 move-only；vector 因 `TimedDataWithEnvironment` move 非 noexcept 而扩容拷贝，不支持
  move-only。故选用 deque 而非吞吐略优的 vector。
- drain 的 `range-for + clear` 与原 `front()/pop_front()` 语义等价（`timeChecker_` 过滤、逐项 move 出值、
  最后统一析构），对任意 `T` 安全。

## 相关文档

- [CommonFlowUtils 节点组合工具](../../../tm_kit_reference/tm_basic/05-common-flow-util.md)（`bunch`/`bunchWithProcessing` 用法）
- [修复条件变量无谓词 1ms 轮询等待](../20261117-fix-cv-1ms-poll/README.md)（同节点的上一步 wait 改造）
- [CommonFlowUtils 自建线程节点适配框架绑核](../20261117-common-flow-utils-cpu-bind/README.md)

## commit

（提交后补）
