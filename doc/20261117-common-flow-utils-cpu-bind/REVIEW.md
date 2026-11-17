# CommonFlowUtils CPU 绑定改动评审报告

**评审日期：** 2026-11-17  
**关联任务：** [README.md](README.md)  
**结论：** 原存在 1 项 P2 问题，已修复。

已结合 infra 的配置分派、CPU 租约及实时节点生命周期评估两个仓库的改动。普通测试连续运行 10 次通过，但新增测试在 AddressSanitizer 下实际触发释放后访问，需修正其生命周期管理（见下文修复）。

## [P2] 避免在工作线程退出前销毁测试节点（已修复）

**位置：** `test/common_flow_utils_cpu_bind_test/CommonFlowUtilsCpuBindTest.cpp:128`（评审时行号）

这里结束作用域会销毁节点，但这些 CommonFlowUtils 节点的线程已经 detach，析构并不会等待线程退出；新测试因此在后台线程仍访问成员时释放对象。使用 `-fsanitize=address` 编译运行该测试，已复现首个 `bunch<int>` 在此处释放后，`BunchAction::runThread()` 读取 `running_` 的 heap-use-after-free。底层析构问题虽已存在，但新增测试主动依赖了不安全的生命周期，会导致随机崩溃或 sanitizer 测试失败。请通过明确的线程退出同步或进程隔离管理测试节点，不要依赖作用域结束或短暂运行来保证安全。

## 修复

选择**进程隔离**（评审给出的两个方案之一）：`BunchAction`/`Delayer`/`LocalAction` 的工作线程均已 `detach`，测试侧无法 `join`，因此把每个节点的两项断言（`CpuAutoBind` + `Cpus`）改到 `fork()` 出的子进程中执行；子进程断言后 `_exit()`，**不销毁节点**——析构与 detach 线程的生命周期竞争随之消失，进程退出时由 OS 回收线程与节点。父进程 `waitpid()` 并汇总子进程退出码。这与仓库既有先例 `test/cpu_cross_process_test/CrossProcessCpuTest.cpp` 的 `::fork()`/`::_exit()`/`::waitpid()` 写法一致。

- 子进程开始处 `failures = 0`（`fork` 继承父进程计数，需清零），只统计本节点的失败。
- 两个节点以 `decltype(makeNode())` 局部变量持有，作用域延续到 `_exit()`（永不析构）；`_exit` 前 `std::cout.flush()` 保证输出落盘。
- `#ifdef __linux__` 之外退化为 skip（绑核本就不支持，与 `affinityOf` 的既有平台守卫一致）。

未采纳"线程退出同步"方案：那需要改动生产代码的析构语义（把 detach 改为可 join 并析构时 join，`Delayer` 可能阻塞至 `duration`），而三个节点的 detach 行为是既有设计（见 [PLAN.md](PLAN.md) 的"保持现状"），且评审明确把该底层析构问题标记为"已存在"、范围限定在新增测试。

## 验证

debug 构建，只编译目标（`-j4`），普通运行与 ASan 运行均通过：

```bash
cd build-common && source ./build-env-setting.sh
ninja -j4 -C <tm_basic>/builddir_debug test/common_flow_utils_cpu_bind_test/common_flow_utils_cpu_bind_test
meson test -C <tm_basic>/builddir_debug common_flow_utils_cpu_bind_test   # OK
```

- 普通运行：8 项断言全过（bunch / bunchWithProcessing / delayer / actionWithBufferedState 各 CpuAutoBind + Cpus），退出码 0。
- ASan（`-fsanitize=address`，`ASAN_OPTIONS=halt_on_error=1:detect_leaks=0`）：8 项断言全过，无 heap-use-after-free，退出码 0。子进程 `_exit` 跳过析构与 LeakSanitizer 的退出检查。

## 跟踪状态

- 已修复：改用 fork 子进程 + `_exit` 隔离节点生命周期，不销毁节点；普通与 ASan 均复测通过。
- 底层析构不 join detach 线程的问题（`BunchAction` 仅置 `running_`、`Delayer`/`LocalAction` 的 `joinable()` 恒 false 死代码）**未改**，属既有缺陷，建议另立任务处理。
