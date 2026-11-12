# CPU 绑定字符串接口代码评审

**评审日期：** 2026-11-12  
**相关任务：** [统一的线程绑核字符串接口](README.md)

## 评审结论

两个仓库新增的测试均通过，但补充验证确认了非法输入被静默接受、none 模板入口编译失败及范围展开溢出三个问题。现有测试未覆盖这些情况。

## 评审意见

### [P2] 完整校验数字 token，拒绝未消费的字符

**位置：** `tm_infra/tm_kit/infra/RealTimeThreadedExecutor.hpp:175-179`

配置包含非法后缀或多余分隔符时，`std::stoi` 只解析数字前缀，导致 `cpus:2foo` 被接受为 CPU 2、`2-3-8` 被接受为 `{2,3}`；NUMA 分支同样把 `numa:1,2` 接受为节点 1。这些输入不会按约定报错，而会静默改变绑定或排除列表，且影响复用此解析器的两个 tm_basic 入口。应检查每次数值转换是否消费了完整 token，并拒绝剩余的非空白字符。

### [P2] 避免 none 路径实例化需要 allocator 的模板

**位置：** `tm_infra/tm_kit/infra/RealTimeThreadedExecutor.hpp:719-724`

当 `Env` 没有 `ThreadCpuAllocatorComponent` 时，调用 `bindCurrentThreadBy<Env>("none", "test")` 仍然编译失败：普通 `switch` 会实例化所有分支，触发三个旧入口的 `static_assert` 和缺失成员错误。这与此接口声明的 none 无需 allocator 相矛盾，也使不启用绑核的环境无法使用统一入口。应通过 `if constexpr` 隔离依赖 allocator 的调用，让 none/空串正常返回，并在实际请求绑定但缺少组件时报告错误。

### [P2] 防止范围展开在 INT_MAX 处溢出

**位置：** `tm_infra/tm_kit/infra/RealTimeThreadedExecutor.hpp:184-186`

输入 `2147483647-2147483647` 时，两个端点都能通过 `stoi`，但循环插入第一个元素后执行 `++c` 会发生有符号整数溢出，已通过 UBSan 复现。普通构建可能继续展开负数并耗尽内存，而不是拒绝配置或终止循环。应在展开前检查端点范围，或采用到达上界后不再递增的循环结构。
