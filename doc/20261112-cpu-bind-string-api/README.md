# 统一的线程绑核字符串接口（tm_basic 侧）

**任务开始时间：** 2026-11-12  
**任务结束时间：** 2026-11-12  
**状态：** 完成  
**分支：** `feature/cpu-bind-string-api`（commit `8a93dd4`）

## 背景

本仓库负责 `HwlocCpuAllocatorConfig` 和 `SharedThreadPoolConfig` 两处需要接受字符串的入口。
主记录在 tm_infra 侧：[20261112-cpu-bind-string-api](../../tm_infra/doc/20261112-cpu-bind-string-api/README.md)

## 本仓库改动

- `HwlocCpuAllocatorConfig::fromString` 静态工厂：解析 `"exclude=8,9;smt=partial;scope=cross;lockdir=..."` → 填充 config 和 scope。
- `SharedThreadPoolConfig::cpusFromString` 静态工厂：复用 tm_infra 的 `parseCpuList` 解析 `"2-5"` / `"2,3,8-11"` → `cpus` 字段。

## 相关文档

- tm_infra 侧主记录：[../../tm_infra/doc/20261112-cpu-bind-string-api/README.md](../../tm_infra/doc/20261112-cpu-bind-string-api/README.md)
- tm_kit_reference：`tm_basic/11-hwloc-cpu-allocator.md`

## 代码评审修复（2026-11-12）

评审报告：[codereview.md](codereview.md)。三个问题都出在 tm_infra 的共享解析器和 `bindCurrentThreadBy<Env>`，修复说明见 [tm_infra 记录](../../../tm_infra/doc/20261112-cpu-bind-string-api/README.md#代码评审修复2026-11-12)。tm_basic 侧的源码不需要改动：`cpusFromString` 和 `HwlocCpuAllocatorConfig::fromString` 直接用上了更严格的 `parseCpuList`。`test/cpu_bind_string_test` 新增 8 个回归用例（`cpus:2foo`、`2-3-8`、`exclude=8foo`、`exclude=-1` 等报错，`exclude=8,9` 仍然接受），debug 构建全部通过。
