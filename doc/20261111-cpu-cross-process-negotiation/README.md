# 多进程 CPU 自协商（CpuAutoBind 跨进程）

**任务开始时间：** 2026-11-11  
**任务结束时间：** 2026-11-11  
**状态：** 完成（未合并 develop，未推送）  
**分支：** `feature/cpu-cross-process-negotiation`（tm_basic 与 tm_infra 均有代码改动），worktree `worktrees/cpu-cross-process-negotiation/`

## 背景
[CpuAutoBind](../../../tm_infra/doc/20261111-thread-cpu-autobind/README.md) 的 `HwlocThreadCpuAllocator` 只在单个进程内防止忙线程抢同一物理核。同一台机器上多个基于 tm_kit 的进程各自自动绑核时，候选顺序相同，busyloop 线程会落到同一个 CPU 上，性能严重下降。

## 目标
- 自动绑核、显式 `Cpus()`、`ThreadCpuLease::bindCurrentThread*` 都参与跨进程协商；
- 线程退出、进程退出（含崩溃、SIGKILL）后释放占用；
- 默认关闭，通过新的 Environment 组件 `HwlocCrossProcessCpuAllocatorComponent` 开启；
- 绑定、解绑在多进程、多线程下并发安全。

## 需求确认（用户答复）
- 开关：新增 Env 组件，替代 `HwlocThreadCpuAllocatorComponent`。
- 冲突：一律独占。显式 `Cpus()` / `bindCurrentThreadTo` 碰到其他进程已占的核也抛 `CpuAutoBindError`。
- 锁机制：比较 shm robust mutex 与 flock 后，用户确认使用 flock（每个物理核一个锁文件）。
- 追加要求：`HwlocCrossProcessCpuAllocatorComponent` 仍支持 `excludeCpus` 避让核心。两种组件共用同一个单例和 `HwlocThreadCpuAllocator::configure()` 配置，`excludeCpus`、`avoidFirstCoreOfNumaNode`、`smt` 在 CrossProcess 下同样生效；被排除的核不参与候选，也不会去加锁文件。

## 方案
- **锁机制**：每个物理核一个锁文件 `<crossProcessLockDir>/core-<N>.lock`，默认目录 `/dev/shm/tm_kit_cpu_locks`（不存在时以 `01777` 创建）。N 为该核完整 cpuset（`HWLOC_TOPOLOGY_FLAG_INCLUDE_DISALLOWED`）中最小的 OS PU，不同 taskset/cgroup 掩码的进程得到同一个键。`open(O_RDWR|O_CREAT|O_CLOEXEC|O_NOFOLLOW, 0666)` + `fchmod(0666)`，`flock(LOCK_EX|LOCK_NB)` 非阻塞加锁；锁文件只创建不删除。锁文件内容 `pid <n> <who>` 仅用于诊断。
- **放弃的方案**：shm + `PTHREAD_MUTEX_ROBUST`。mutex 能在持有者死亡后恢复，但共享内存里的占用表需要按 pid 存活检查回收，pid 复用和跨 pid namespace 都会出错；flock 由内核在 fd 关闭时释放，锁本身就是占用状态，无残留。`fcntl(F_SETLK)` 也未采用：它按进程计，进程内关闭同一文件的任意 fd 会释放全部锁。
- **进程内**：每核只打开一个 fd，按引用计数管理；进程内互斥仍由 `mutex_` 与 `assignedCpus_` / `reservedCpus_` / `reservedBy_` 负责，flock 只负责跨进程。
- **开关**：`HwlocCrossProcessCpuAllocatorComponent` 与 `HwlocThreadCpuAllocatorComponent` 共用 `HwlocThreadCpuAllocator::instance()` 单例和配置，首个使用的组件确定 scope，混用抛 `std::logic_error`。两种 scope 的规则一致，区别只在冲突检测范围（进程内 / 同机所有协商进程）。
- **fork**：`O_CLOEXEC` 防止 exec 泄漏；`pthread_atfork` child 回调遍历固定容量的 `std::atomic<int>` fd 槽位并 `close()`（不加 `mutex_`），子进程不会延长占用。
- **excludeCpus**：CrossProcess 下对排除核加锁并持有到进程退出，其他进程随之避开；`configure(cfg, CpuAllocatorScope::CrossProcess)` 或组件首次取分配器时立即发布。两种 scope 下显式绑定到排除核都抛异常。
- **NUMA 排序**：CrossProcess 下读取 `/proc/locks` 得到其他进程持有的核，只用于节点排序和诊断，分配以 try-lock 为准。

## 代码改动（按文件列出）
- tm_infra `tm_kit/infra/RealTimeThreadedExecutor.hpp`：`IThreadCpuAllocator` 新增虚函数 `reserveReplacing(cpus, previous, who)`，默认实现转调 `reserve()`，已有自定义分配器无需修改。
- tm_infra `tm_kit/infra/RealTimeApp.hpp`：`ExecHandlerCore::configureWithLease` 显式 `Cpus()` 改调 `reserveReplacing`，把节点当前租约的 CPU 作为 `previous`，同一节点重配置到自己的核不算冲突。
- tm_infra `test/threaded_handler_test/ThreadedNodeOptionsTest.cpp`：`reserveReplacing` 默认实现与 previous 传递的用例。
- tm_basic `tm_kit/basic/HwlocThreadCpuAllocator.hpp`：
  - `HwlocCpuAllocatorConfig::crossProcessLockDir`；枚举 `CpuAllocatorScope { Local, CrossProcess }`；
  - 构造函数增加 scope 参数（独立实例用）；`instanceFor(scope)`、`scope()`、`configure(cfg, scope)`；
  - `acquire` 按候选顺序 try-lock，跳过他进程持有的核；`reserve` / `reserveReplacing` 检查 excludeCpus、进程内显式与自动占用，CrossProcess 下加锁失败整体回滚；`release` 引用计数归零时 close；
  - 锁辅助：`tryLockCore` / `unlockCore` / `closeAllLocks` / `publishExclusions` / `refreshForeignBusy`，fork 槽位与 `pthread_atfork`；
  - 新组件 `HwlocCrossProcessCpuAllocatorComponent`。Linux 专用代码在 `#ifdef __linux__` 内，其他平台使用 CrossProcess 抛 `CpuAutoBindError`。
- tm_basic `test/hwloc_cpu_allocator_test/HwlocCpuAllocatorTest.cpp`：excludeCpus 显式拒绝、`explicitExclusiveTest(local/cross)`、`graphExplicitExclusiveTest`。
- tm_basic `test/cpu_cross_process_test/`（新增）：`CrossProcessCpuTest.cpp`、`run_cross_process.sh`、`meson.build`；`test/meson.build` 在 hwloc 可用时加入。

## 测试与验证
- 2026-11-11 冒烟（g++ 直接编译单个 TU，CrossProcess 组件，私有锁目录）：未配置排除时 `bindCurrentThread` 拿到 CPU 2，生成 `core-2.lock`；配置 `excludeCpus = {2}` 后拿到 CPU 3，只生成 `core-3.lock`。CPU 2 的 SMT 兄弟是 30，整核都被避开。
- 2026-11-11 新增 `test/cpu_cross_process_test/`（`CrossProcessCpuTest.cpp` 多模式辅助程序 + `run_cross_process.sh`，锁目录用 `mktemp` 私有目录，经 `TM_CPU_LOCK_DIR` 传入，不碰默认 `/dev/shm/tm_kit_cpu_locks`；meson 中 `is_parallel: false`，仅在 hwloc 可用时加入）。覆盖场景：
  1. 8 个进程同时 `bindCurrentThread`，物理核两两不同，其他进程看到的剩余核数相应减少，进程退出后全部恢复；
  2. 压力：4 进程 × 6 线程 × 200 轮 bind/reset，用 mmap 共享的每核计数器检测重叠，overlaps=0；
  3. 占满全部核的进程被 `kill -9` 后，所有核立即可再分配；
  4. 线程结束后其核在进程存活期间即被释放；
  5. 显式独占：B 对 A 自动占用的 CPU 及其 SMT 兄弟 `bindCurrentThreadTo` 抛异常，消息含 A 的 pid；B 显式占用后，其他进程的自动分配避开该核；
  6. excludeCpus 跨进程生效：E 排除的核和 E 自己线程的核都被其他进程跳过，E 退出后恢复；要排除的核已被他人占用时抛异常；
  7. fork 不 exec 的子进程存活时，父进程释放的核可被他人拿到；
  8. `taskset -c 2,30` 的进程与无约束进程使用同一个锁键；
  9. 图节点 `CpuAutoBind()` 参与协商；
  10. Local 组件下两个进程拿到同一 CPU（默认行为不变）且不持有锁；同一进程混用两种组件抛 `logic_error`。
- 踩坑：脚本的 `start` 总是把参数交给测试程序，taskset 场景传入的 `bash -c ...` 被当成模式名而失败；改为带命令前缀的 `start_wrapped`。
- 构建：`source build-common/build-env-setting.sh`（GCC 15.2.1），worktree 私有 `builddir_release` / `builddir_debug`，`ninja -j2` 只编译 `hwloc_cpu_allocator_test`、`hwloc_cpu_allocator_constraints`、`cpu_cross_process_test` 三个目标，无相关警告。
- Release `meson test`（三项）：3/3 通过；`run_cross_process.sh` 连跑 5 次均 ALL PASSED。
- Debug `meson test`（三项）：3/3 通过。
- TSan（`-O1 -fsanitize=thread`）：多进程脚本 ALL PASSED；用 `TSAN_OPTIONS=log_path=` 把所有子进程的报告写到固定目录，目录为空，无数据竞争报告。

- 依赖禁用：`-Dhwloc_cpu_allocator=disabled` 的独立构建目录 `builddir_hwloc_off` 配置、编译通过，`cpu_cross_process_test` 等 hwloc 目标被跳过；`-DTM_KIT_BASIC_DISABLE_HWLOC_CPU_ALLOCATOR=1` 下两个测试源文件 `-fsyntax-only` 通过。

## 兼容性与注意事项
- 默认行为不变：只挂 `HwlocThreadCpuAllocatorComponent` 时不创建也不持有锁文件。
- **不兼容点（用户确认）**：
  - 显式 `Cpus()`（Environment 挂分配器时）/ `bindCurrentThreadTo` 绑到 `excludeCpus` 所在物理核，由成功变为抛 `CpuAutoBindError`；
  - 进程内两个显式绑定落在同一物理核，由成功变为抛异常。同一节点用相同 CPU 重配置不受影响。
- infra 只追加了带默认实现的虚函数 `reserveReplacing`，源码兼容；自定义分配器若希望重配置到自身核不报冲突，需要覆盖它。
- 只有使用同一锁目录的进程之间才会协商；不同容器的 `/dev/shm` 互相隔离，需挂载同一目录。不支持 NFS 等网络文件系统。
- 不使用 tm_kit 的第三方进程不参与协商，仍需 `excludeCpus` 避让。
- 能写锁目录的本机用户都可以占住锁文件，目录权限由部署方负责。
- 只 fork 不 exec 的子进程：继承的锁在子进程中关闭；`ThreadCpuLease` 线程登记表随 fork 复制，子进程应在新线程里绑核。

## 相关文档
- [节点线程自动绑核（CpuAutoBind）](../../../tm_infra/doc/20261111-thread-cpu-autobind/README.md)
- tm_kit_reference：`tm_infra/10-realtime-executors.md`「自动绑核」

### 追加：excludeCpus 对其他进程生效（2026-11-11）
- 用户要求：CrossProcess 模式下，一个进程用 `excludeCpus` 避让的核，其他进程也要避让。
- 实现：`publishExclusions()` 在首次 acquire / reserve 时，对 `excludeCpus` 涉及的每个物理核加 flock，锁文件内容写 `excludeCpus`。锁持有到进程退出，进程无论怎么退出，内核都会释放锁。其他进程的 try-lock 失败后会把这些核当作已占用，跳过它们。
- 发布失败：某个排除核已被其他进程占用时，无法替对方腾出这个核，因此回滚本次已加的锁，然后抛 `CpuAutoBindError`。下次调用会重新尝试。
- 本进程对自己排除的核做显式 `Cpus()` / `bindCurrentThreadTo` 时，走同一把锁的引用计数，不会和自己冲突。
- fork 不 exec 的子进程会关闭继承来的锁，所以按 pid 判断，子进程中重新发布。
- Local 模式不变，`excludeCpus` 仍只影响本进程。
- 冒烟（g++ 单 TU，私有锁目录）：B 单独运行拿到 CPU 2；A 配置 `excludeCpus={2}` 并存活时，A 自己拿到 CPU 3，B 拿到 CPU 4，避开了 2 和 3；A 退出后 B 重新拿到 CPU 2。

### 追加：显式绑定也遵守 excludeCpus（2026-11-11，不兼容变更，用户确认）
- 用户要求 Local 与 CrossProcess 行为一致：`excludeCpus` 排除的核，其他进程不能用（CrossProcess 通过锁发布），本进程内也不能通过任何方式绑定。
- `reserve()` 新增检查：CPU 所在物理核含 `excludeCpus` 中的 CPU 时抛 `CpuAutoBindError`（消息含 `excludeCpus` 列表），两种 scope 都生效。覆盖 `Cpus()`（Environment 带分配器时）与 `ThreadCpuLease::bindCurrentThreadTo`。按整核判断，SMT 兄弟同样拒绝；列表中任一 CPU 被拒则整次调用不登记任何预留。
- 撤销此前“本进程可显式绑到自己排除的核”的做法。
- 不兼容点：已有代码若显式绑定到 `excludeCpus` 中的核，会从静默成功变为抛异常。Environment 未挂分配器组件时 `Cpus()` 不经过分配器，不受影响。
- 测试：
  - `hwloc_cpu_allocator_test` excludeTest：排除核及其兄弟 `reserve()` 抛异常；非排除核正常；含排除核的列表整体拒绝且不残留；`ThreadCpuLease::bindCurrentThreadTo` 抛异常。
  - `cpu_cross_process_test` 新模式 `selfexcl <local|cross> <lease|graph> <cpu>`：两种 scope × lease / 图节点 `Cpus()`，排除 CPU 2 及兄弟 30 均被拒绝。
  - Release、Debug 各 3/3 通过。

## Local / CrossProcess 一致性修正（2026-11-11）
用户要求两种组件只在“进程内 / 跨进程”范围上不同，其他行为一致。审查后修正 4 处：
1. **显式绑定进程内也独占**：`reserve()` 现在检查 `reservedBy_`，同一物理核已被另一显式预留占用时抛 `CpuAutoBindError`，消息带持有者。为了让节点用相同 CPU 重配置不算冲突，tm_infra `IThreadCpuAllocator` 新增虚函数 `reserveReplacing(cpus, previous, who)`（默认实现转调 `reserve`，旧的自定义分配器无需修改），`ExecHandlerCore::configureWithLease` 把节点当前租约的 CPU 作为 `previous` 传入。**不兼容点**：进程内两个显式绑定重叠以前成功，现在抛异常。
2. **NUMA 选择计入其他进程的占用**：CrossProcess 下 `acquire` 前读取 `/proc/locks` 生成其他进程持有的核快照 `foreignBusy_`，只用于节点排序和空闲核计数；不用试探加锁，避免让并发进程误跳过空闲核。分配仍以 flock try-lock 为准，快照中标为占用的核排在最后仍会尝试。
3. **fork 子进程配置 excludeCpus 后可正常绑核**：子进程发现排除核由祖先进程的 excludeCpus 锁持有时视为已生效（`acceptedExclusionHolders_`），待祖先退出后再补加锁。
4. **excludeCpus 配置后立即对其他进程生效**：新增 `HwlocThreadCpuAllocator::configure(cfg, CpuAllocatorScope)`，CrossProcess 下在返回前加锁发布；组件首次取分配器（`requireScope`）时也会发布。冲突时抛异常并回滚配置。

## 测试覆盖盘点（2026-11-11）
| 特性 | 用例位置 |
|---|---|
| 多进程自动绑核互斥 | cross 场景 1、2（压力） |
| 进程退出（含 kill -9）、线程退出释放 | cross 场景 3、4 |
| 显式绑定跨进程独占、SMT 兄弟 | cross 场景 5 |
| 显式绑定进程内独占（两种 scope、8 线程竞争、列表整体回滚） | unit `explicitExclusiveTest(local/cross)` |
| 图节点 Cpus() 重配置到自身 CPU 允许、他人冲突拒绝 | unit `graphExplicitExclusiveTest` |
| `reserveReplacing` 默认实现与 previous 传递 | infra `threaded_node_options_test` |
| excludeCpus 跨进程避让、本进程显式拒绝（lease/graph × local/cross） | cross 场景 6 |
| excludeCpus 仅 configure 即生效、冲突抛异常 | cross 场景 11（`exclnow`） |
| fork 不延长占用；fork 子进程 + excludeCpus 正常绑核 | cross 场景 7、12（`forkexcl`） |
| 不同 taskset 掩码共用锁键 | cross 场景 8 |
| 图节点 CpuAutoBind 参与协商 | cross 场景 9 |
| NUMA 选择考虑他进程占用（节点 0 被占满后 4 线程都落在节点 1） | cross 场景 13（`fillnuma`） |
| 默认 Local 不变、混用组件抛 logic_error | cross 场景 10 |

踩坑：`forkexcl` 子进程直接在继承的线程上调用 `bindCurrentThread`，ThreadCpuLease 的线程登记表随 fork 复制，误报“已持有租约”；测试改为子进程里新起线程绑定（与真实使用一致）。NUMA 场景最初用 `hold 4 0`，线程绑定后立即释放导致 CPU 重复（3 5 3 5），改为持有 2 秒并加 distinct_cores 检查。

结果（infra 头文件装到 worktree 私有前缀 `install_private`，tm_basic 以此重新配置）：
- tm_infra Release / Debug：4/4。
- tm_basic Release / Debug：`hwloc_cpu_allocator_test`、`hwloc_cpu_allocator_constraints`、`cpu_cross_process_test` 3/3。
- TSan（`-O1 -fsanitize=thread`）：两个测试程序 ALL PASSED，报告目录为空。
