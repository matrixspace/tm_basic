# 计划：CommonFlowUtils 自建线程节点适配框架绑核

## Context（背景）

`tm_basic/tm_kit/basic/CommonFlowUtils.hpp` 里若干节点在 `RealTimeApp` 模式下**自己 `std::thread` 起线程并 `detach()`**，不走框架 executor，因此 `M::configureThreadedNode(node, ThreadedNodeOptions)` 对它们**无效**（`IRealTimeAppPossiblyThreadedNode::configureThread` 默认返回 `false`）。目标是让这些节点复用框架绑核能力（`Cpus()` / `CpuAutoBind()` / `CpuAutoBindOnNuma()` / 字符串 `CpuBind`）。

范围：CommonFlowUtils 内四个工厂函数，共用三类内部线程类：

| 工厂函数 | 内部类 | 当前状态 |
|---|---|---|
| `bunchWith<T>()` / `bunchWithProcessing<T,Buncher>()` | `BunchAction` (`:190`) | 继承接口，`threadHandle()` 在 detach 后取 `native_handle()` → 返回 0（坏）；无 `configureThread` |
| `delayer<T>(duration)` | `Delayer` (`:1555`) | **未**继承接口、无 `threadHandle()`、无 `configureThread` |
| `actionWithBufferedState<...>()` | `LocalAction` (`:2269`) | 继承接口，`threadHandle()` detach 后取 → 返回 0（坏）；无 `configureThread` |

**已确认关键事实**：libstdc++ `std::thread::detach()` 会把 `_M_id._M_thread` 置 0，之后 `native_handle()` 返回 0。因此必须**在 detach 前缓存** `native_handle_type`。仓库先例：`tm_transport/src/singlecast/SinglecastComponent.cpp:419`、`SharedMemoryBroadcastComponent.cpp:91`、infra `DedicatedThreadExecutor` 的 `handle_` 缓存（`RealTimeThreadedExecutor.hpp:444/509`）。

**承载方式（已选定）**：方案 D —— 把绑核逻辑**下沉到 infra 的模板 helper**（`ThreadCpuBinding<StateT>`），节点各加一个成员 + 薄覆盖 `configureThread`。不改 `IRealTimeAppPossiblyThreadedNode` 接口（纯增量、兼容）；同时把 `setThreadAffinity`/`validCpus`/`HasThreadCpuAllocator` 这些 detail 符号的使用**留在 infra 内部**，避免 tm_basic 跨库触碰 infra detail 命名空间。

参考实现：`ExecHandlerCore::configureThread` + `configureWithLease`（`tm_infra/tm_kit/infra/RealTimeApp.hpp:347-408`）。唯一差异是"应用亲和性"那一步：executor 走 `exec_.configure()`，裸线程走 `setThreadAffinity(handle, cpus)`。

## 方案

### 1. infra 新增 `ThreadCpuBinding<StateT>`（`tm_infra/tm_kit/infra/RealTimeThreadedExecutor.hpp`，放在 `ThreadCpuLease` 之后）

职责 = allocator 解析 + 租约管理 + 按 native handle 绑核。持 `std::mutex leaseMutex_` 和 `infra::ThreadCpuLease lease_`；语义逐字复刻 `ExecHandlerCore::configureThread`，仅把 `realtime_threaded_executor_detail::configure(exec_, applied, 0)` 换成 `realtime_threaded_executor_detail::setThreadAffinity(handle, cpus)`：

```cpp
template <class StateT>
class ThreadCpuBinding {
    std::mutex leaseMutex_;
    ThreadCpuLease lease_;
    template <class S = StateT>
    bool configureWithLease(ThreadedNodeOptions const&, std::vector<int>, bool acquired,
                            std::thread::native_handle_type handle, std::string const& who);
public:
    bool configure(ThreadedNodeOptions const &o,
                   std::optional<std::thread::native_handle_type> const &handle,
                   std::string const &who);
};
```

`configure` 语义（与 `ExecHandlerCore::configureThread` 一致）：

1. `cpuAutoBind && !cpus.empty()` → 抛 `std::invalid_argument`。
2. `!cpuAutoBind`：有分配器组件且 `cpus` 非空 → `configureWithLease(o, o.cpus, false, *handle, who)`；否则 `setThreadAffinity(*handle, o.cpus)`（空 cpus = no-op；`handle` 为空返回 false）。
3. `cpuAutoBind`：无分配器组件 → 抛 `RealTimeAppException`；`handle` 为空 → 抛；否则 `StateT::threadCpuAllocator().acquire(*o.cpuAutoBind, who)` → `configureWithLease(o, {cpu}, true, ...)`。

`configureWithLease`：`validCpus` 校验、`reserveReplacing(cpus, previous=lease_.cpus(), who)`、`ThreadCpuLease::adopt(allocator, cpus)`、`setThreadAffinity(handle, cpus)`；失败时 `acquired` 抛异常 / 否则返回 `false`；成功 `lease_.swap(next)`。`spinWait` 忽略（裸线程无自旋，与 `DedicatedThreadExecutor` "忽略 spinWait 但返回 true"一致）。`template <class S = StateT>` 保证 `S::threadCpuAllocator()` 仅在被调用时实例化（与 `ExecHandlerCore` 同款写法）。

**不动 `ExecHandlerCore`**：executor 路径"应用亲和性"含 `spinWait`/`ExecConfigurable`/`kOwnContext`/Inline/SharedPool 拒绝逻辑，与裸线程不同，本次不重构它，接受 ~30 行 allocator 解析逻辑的轻微重复（后续可再考虑用 apply-callback 泛化后统一）。

### 2. CommonFlowUtils 三个节点改动（统一模式，各 ~5 行）

每个节点：

- 加成员 `infra::ThreadCpuBinding<TheEnvironment> cpuBinding_;` 和 `std::optional<std::thread::native_handle_type> handle_;`（**声明在 `std::thread` 成员之前**，保证租约在线程对象之后析构）。
- 构造函数里 `detach()` 之前：`handle_ = th_.native_handle();`。
- `threadHandle()` 覆盖改为返回 `handle_`（修掉返回 0 的既有 bug）。
- 加薄覆盖 `configureThread`：
  ```cpp
  virtual bool configureThread(infra::ThreadedNodeOptions const &o) override final {
      return cpuBinding_.configure(o, handle_, /*who=*/typeid(*this).name());
  }
  ```

具体：

- **`BunchAction`** (`:190`)：已有接口继承与 `threadHandle()` 覆盖；改 `threadHandle()` 返回 `handle_`，加 `cpuBinding_`/`handle_`/`configureThread`。
- **`LocalAction`** (`:2269`)：同上。
- **`Delayer`** (`:1555`)：补 `public infra::IRealTimeAppPossiblyThreadedNode` 基类 + 上述全套（其析构 `if (th_.joinable()) th_.join()` 因 detach 后恒 false 是既有死代码，本次不动）。

节点改完后，`M::configureThreadedNode` / `M::threadHandle` 经 `node->threadInfo_`（`dynamic_cast<IRealTimeAppPossiblyThreadedNode*>`）即自动命中。

## 代码改动清单

- `tm_infra/tm_kit/infra/RealTimeThreadedExecutor.hpp`：新增 `ThreadCpuBinding<StateT>`（public，`ThreadCpuLease` 之后）。纯头文件，无 meson/`.pc` 变更。
- `tm_basic/tm_kit/basic/CommonFlowUtils.hpp`：改 `BunchAction`、`Delayer`、`LocalAction`；确认/补齐 `<mutex>` `<thread>` `<sstream>` include（现为传递包含，显式加上更稳）。
- `tm_basic/test/common_flow_utils_cpu_bind_test/CommonFlowUtilsCpuBindTest.cpp`（新增）
- `tm_basic/test/common_flow_utils_cpu_bind_test/meson.build`（新增）
- `tm_basic/test/meson.build`：在 `if have_hwloc_cpu_allocator`（Linux 块内）`subdir` 注册新测试。

## 测试与验证

仿照 `tm_basic/test/hwloc_cpu_allocator_test/`（其 `affinityOf(handle)` 用 `pthread_getaffinity_np`，构造节点 → `configureThreadedNode` → 断言亲和性，不跑 `execute()`）。

- Env：`infra::Environment<..., basic::HwlocThreadCpuAllocatorComponent>`；`main()` 先 `basic::HwlocThreadCpuAllocator::configure({})`。
- 对三个节点分别：
  1. 构造节点（线程在构造函数启动）。
  2. `M::configureThreadedNode(node, ThreadedNodeOptions().CpuAutoBind())`，断言返回 `true` 且 `affinityOf(*M::threadHandle(node)).size() == 1`。
  3. 再测 `Cpus({cpu})` 显式绑核 + 断言。
  4. 作用域结束即析构（不泄漏：`actionWithBufferedState` 线程 100ms 后若 `env_==nullptr` 会解引用空指针，尽快析构即可）。
- `bunchWith`/`bunchWithProcessing` 共用 `BunchAction`，用一个 `bunchWithProcessing<int, 简单 Buncher>` 覆盖；再补一个带 `isFinal()` 的 T 实例化 `bunchWith`。
- `actionWithBufferedState` 用最小 `StateUpdater`/`OutputProducer`/`StateResetter`（仅编译通过；测试不喂数据，线程不触发）。
- （可选，低代价）`tm_infra/test/` 加一个用 mock `IThreadCpuAllocator` 的 `ThreadCpuBinding` 单测（无需 hwloc），覆盖 `Cpus` 预留 / `CpuAutoBind` acquire / 析构 release / 冲突抛异常。分配器解析语义已由现有 `hwloc_cpu_allocator_test` 的 executor 路径覆盖，此项非必需。

构建（只编目标、限 `-j`）：

```bash
cd build-common && source ./build-env-setting.sh
# 首次/依赖变更后 meson setup --reconfigure；无 hwloc 时本测试被跳过、头文件仍编译
ninja -j4 -C <tm_basic>/builddir_debug common_flow_utils_cpu_bind_test
meson test -C <tm_basic>/builddir_debug common_flow_utils_cpu_bind_test
```

## 兼容性与注意事项

- 纯增量：新增 infra public 类型 `ThreadCpuBinding`；不改 `IRealTimeAppPossiblyThreadedNode` 接口，不破坏任何下游。
- `threadHandle()` 语义变化：从"返回 0"变为"返回 detach 前缓存的 handle"，在合法调用窗口（节点创建后、`finalize()` 前）内有效。
- 这些节点线程仍 detached（保持现状）；`cpuBinding_` 声明在线程成员之前，租约释放顺序正确。`BunchAction` 不 join，释放时线程可能再存活 <1ms（既有行为，不改）。
- `spinWait` 被忽略（与 `DedicatedThreadExecutor` 一致）。
- 不依赖 hwloc：helper 只用 infra 的 `IThreadCpuAllocator`/`setThreadAffinity`；`CpuAutoBind()` 需环境挂分配器组件时才可用（与 executor 路径一致）。

## 文档维护（AGENTS.md 必做）

- 开工前 `tm_basic/doc/20261117-common-flow-utils-cpu-bind/README.md`（状态"进行中"），结束前补全"完成" + 分支/commit；更新 `tm_basic/doc/INDEX.md`。
- Git Flow：从 `develop` 开新分支，**不碰 master**；代码与文档一起提交。
- 推送后同步 `tm_kit_reference`（`tm_infra/10-realtime-executors.md` 新增 `ThreadCpuBinding` 说明、`tm_basic/05-common-flow-util.md` 相关段落），确认后推送。

## 后续（不在本次范围，仅记录）

- 同类缺口节点：`SimpleImporter`/`UniformSimpleImporter`/`BunchedImporter`/两个 `LocalI`（`RealTimeApp.hpp:2201/2246/2313/2406/2512`）、tm_basic `ChainReader/ChainWriter`，同样只覆写 `threadHandle()` 不覆写 `configureThread`，可复用 `ThreadCpuBinding` 接入（同样"缓存 handle + 薄覆盖"）。
- `ExecHandlerCore` 若后续也想复用，可给 `ThreadCpuBinding` 加 apply-callback 泛化后统一（本次不做，避免动 executor 热路径）。
- `CommonFlowUtils.hpp:1803` 的 `.SuggestThreadedcollectAn(...)` 笔误；`simpleMovingTimeWindowKleisli`/`wholeHistoryFold` 与 `tm_kit_reference` 文档 `suggestThreaded` 参数描述不符——均独立成 bug 另行处理。
