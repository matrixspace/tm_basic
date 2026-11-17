# CommonFlowUtils 自建线程节点适配框架绑核

**任务开始时间：** 2026-11-17
**任务结束时间：** 2026-11-17
**状态：** 完成
**分支：** `feature/common-flow-utils-cpu-bind`（tm_basic commit `72289a9`，tm_infra commit `9a13b1e`）

## 背景

`CommonFlowUtils.hpp` 里 `bunchWith`/`bunchWithProcessing`、`delayer`、`actionWithBufferedState` 在 `RealTimeApp` 模式下会自己 `std::thread` 起线程并 `detach()`，不走框架 executor，因此 `M::configureThreadedNode(...)` 对它们无效。目标是让这些节点复用框架绑核能力（`Cpus()` / `CpuAutoBind()` / `CpuAutoBindOnNuma()` / 字符串 `CpuBind`）。

同时发现并顺带修复：这些节点的 `threadHandle()` 在 `detach()` 之后取 `native_handle()`，而 libstdc++ 的 `detach()` 会清空句柄，导致一直返回 0。

## 方案（含被放弃的方案和原因）

详细方案见 [PLAN.md](PLAN.md)。

要点：把绑核逻辑下沉到 infra 的模板 helper `infra::ThreadCpuBinding<StateT>`（新增，放在 `RealTimeThreadedExecutor.hpp` 的 `ThreadCpuLease` 之后），节点各加一个成员 + 薄覆盖 `configureThread`，并在 `detach()` 前缓存 `native_handle`。

被放弃/未采用的方案：
- 下沉进非模板接口 `IRealTimeAppPossiblyThreadedNode`（需运行时注入 allocator、接口变有状态、executor 路径仍要单独覆盖）。
- 接口模板化 `IRealTimeAppPossiblyThreadedNode<StateT>`（破坏公共接口，波及 11 个节点类与下游）。
- 仅在本文件内加 mixin（无法惠及 infra 其他自建线程节点）。

## 代码改动（按文件列出）

- `tm_infra/tm_kit/infra/RealTimeThreadedExecutor.hpp`：新增 `ThreadCpuBinding<StateT>`。
- `tm_basic/tm_kit/basic/CommonFlowUtils.hpp`：`BunchAction`、`Delayer`、`LocalAction` 三个类适配。
- `tm_basic/test/common_flow_utils_cpu_bind_test/`（新增测试）。

## 测试与验证（命令、构建类型、结果）

debug 构建，只编译相关目标（`-j4`）：

```bash
cd build-common && source ./build-env-setting.sh
# tm_infra：meson setup + ninja + install（安装含 ThreadCpuBinding 的新头文件）
# tm_basic：ninja -C builddir_debug test/common_flow_utils_cpu_bind_test/common_flow_utils_cpu_bind_test
meson test -C builddir_debug common_flow_utils_cpu_bind_test hwloc_cpu_allocator_test cpu_bind_string_basic_test
```

结果：新增 `common_flow_utils_cpu_bind_test` 8 项断言全过（bunch / bunchWithProcessing / delayer / actionWithBufferedState 各测 CpuAutoBind + Cpus）；既有 `hwloc_cpu_allocator_test`、`cpu_bind_string_basic_test` 通过，无回归。

## 兼容性与注意事项

纯增量：新增 infra public 类型 `ThreadCpuBinding`，不改 `IRealTimeAppPossiblyThreadedNode` 接口，不破坏下游。

## 相关文档（相关任务、tm_kit_reference 章节）

- [tm_infra/20261111-thread-cpu-autobind](../../tm_infra/doc/20261111-thread-cpu-autobind/README.md)
- [tm_infra/20261112-cpu-bind-string-api](../../tm_infra/doc/20261112-cpu-bind-string-api/README.md)
- tm_kit_reference：`tm_infra/10-realtime-executors.md`、`tm_basic/05-common-flow-util.md`
