# CommonFlowUtils CPU 绑定改动评审报告

**评审日期：** 2026-11-17  
**关联任务：** [README.md](README.md)  
**结论：** 存在 1 项 P2 问题，待修复。

已结合 infra 的配置分派、CPU 租约及实时节点生命周期评估两个仓库的改动。普通测试连续运行 10 次通过，但新增测试在 AddressSanitizer 下实际触发释放后访问，需修正其生命周期管理。

## [P2] 避免在工作线程退出前销毁测试节点

**位置：** `test/common_flow_utils_cpu_bind_test/CommonFlowUtilsCpuBindTest.cpp:128`（评审时行号）

这里结束作用域会销毁节点，但这些 CommonFlowUtils 节点的线程已经 detach，析构并不会等待线程退出；新测试因此在后台线程仍访问成员时释放对象。使用 `-fsanitize=address` 编译运行该测试，已复现首个 `bunch<int>` 在此处释放后，`BunchAction::runThread()` 读取 `running_` 的 heap-use-after-free。底层析构问题虽已存在，但新增测试主动依赖了不安全的生命周期，会导致随机崩溃或 sanitizer 测试失败。请通过明确的线程退出同步或进程隔离管理测试节点，不要依赖作用域结束或短暂运行来保证安全。

## 跟踪状态

- 待修复；本次仅归档评审报告，未修改代码或重新执行测试。
- 上述测试结果来自评审输出；原输出未提供完整 sanitizer 构建命令及日志。
