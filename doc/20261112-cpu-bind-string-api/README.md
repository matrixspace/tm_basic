# 统一的线程绑核字符串接口（tm_basic 侧）

**任务开始时间：** 2026-11-12  
**任务结束时间：** 2026-11-12  
**状态：** 完成  
**分支：** `feature/cpu-bind-string-api`（commit 待填）

## 背景

本仓库负责 `HwlocCpuAllocatorConfig` 和 `SharedThreadPoolConfig` 两处需要接受字符串的入口。
主记录在 tm_infra 侧：[20261112-cpu-bind-string-api](../../tm_infra/doc/20261112-cpu-bind-string-api/README.md)

## 本仓库改动

- `HwlocCpuAllocatorConfig::fromString` 静态工厂：解析 `"exclude=8,9;smt=partial;scope=cross;lockdir=..."` → 填充 config 和 scope。
- `SharedThreadPoolConfig::cpusFromString` 静态工厂：复用 tm_infra 的 `parseCpuList` 解析 `"2-5"` / `"2,3,8-11"` → `cpus` 字段。

## 相关文档

- tm_infra 侧主记录：[../../tm_infra/doc/20261112-cpu-bind-string-api/README.md](../../tm_infra/doc/20261112-cpu-bind-string-api/README.md)
- tm_kit_reference：`tm_basic/11-hwloc-cpu-allocator.md`
