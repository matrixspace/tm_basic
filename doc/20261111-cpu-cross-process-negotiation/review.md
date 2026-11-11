# CPU 跨进程协商评审报告

**评审日期：** 2026-11-11  
**来源：** 本次会话提供的 reviewer model 完整评审结果  
**状态：** 4 项 P2 均已修复并补充测试，见文末“修复记录”。  
**评审代码：** `worktrees/cpu-cross-process-negotiation/tm_basic/tm_kit/basic/HwlocThreadCpuAllocator.hpp`

## 总体结论

现有 Release 节点选项测试和跨进程测试通过，但最小程序确认了异常回滚、并发 fork 和排除锁继承中的三个缺陷。此外，已有锁文件的打开方式会阻碍部分 Linux 配置下的跨用户协商。

## 评审意见

### 1. [P2] 在获取核锁抛异常时回滚此前取得的锁

**位置：** `HwlocThreadCpuAllocator.hpp:280-286`

当多核预留已锁住第一个核，而后续 `tryLockCore()` 因文件权限、文件类型或 fd 耗尽而抛异常时，这里的回滚不会执行；它只处理返回 false 的情况。此时 `reservedCpus_` 尚未登记，调用 `release()` 也无法释放残留锁，其他进程会持续认为这些核被占用。已用第二个锁文件路径为目录的场景复现。应通过异常安全的回滚守卫覆盖所有失败出口，`publishExclusions()` 中相同的累积加锁流程也需要处理。

### 2. [P2] 将锁 fd 的整个生命周期与 fork 同步

**位置：** `HwlocThreadCpuAllocator.hpp:901-904`

如果另一个线程在 `open()` 后、`addSlot()` 前执行 fork，子进程会继承尚未登记的 fd，child 回调无法关闭它；即使父进程释放租约，该核也会一直被子进程占用。已通过在 flock 成功后暂停获取线程再 fork 稳定复现，子进程退出后锁才释放。`removeSlot()` 与 `close()` 之间也有同类窗口。原子槽位只能避免数据竞争，必须将打开/登记和取消登记/关闭与 fork 序列化。

### 3. [P2] 确保祖先进程退出后子进程的排除核仍受保护

**位置：** `HwlocThreadCpuAllocator.hpp:996-998`

当配置了 `excludeCpus` 的父进程 fork 后，子进程在父进程存活期间成功绑定，这里只接受父进程的排除锁，并未取得自己的持有权。如果父进程随后退出，排除锁立即消失；子进程只要不再调用 acquire/reserve，就永远不会补锁。已复现子进程仍存活并持有租约时，第三个分配器可以成功预留其排除核。需要保证排除锁在子进程生命周期内连续有效，而不能依靠下一次分配时重试。

### 4. [P2] 分开打开已有锁文件和创建新锁文件

**位置：** `HwlocThreadCpuAllocator.hpp:886-890`

在启用 `fs.protected_regular=1/2` 的 Linux 上，默认的 01777 锁目录中，如果某个已有锁文件既不属于调用用户，也不属于目录所有者，即使文件权限为 0666，带 `O_CREAT` 的打开仍会返回 EACCES。例如第二个用户创建核锁后，第一个用户再次分配经过该核就会抛异常，而不是正常检查锁或跳过占用核。后面的 `fchmod()` 无法修复这个问题。应不带 `O_CREAT` 打开已有文件，仅在文件不存在时走独立的创建及竞争重试流程。

## 相关文档

- [当前开发 worktree 的任务记录](../../../worktrees/cpu-cross-process-negotiation/tm_basic/doc/20261111-cpu-cross-process-negotiation/README.md)

## 修复记录（2026-11-11）

| # | 修复 | 测试 | 旧代码下 |
|---|---|---|---|
| 1 | `reserveReplacing` 的跨进程加锁循环、`publishExclusions` 用 try/catch 包住，任何异常都回滚本次已取得的锁后再抛出 | cross 场景 14（`rollback`：第二个核的锁文件路径是目录） | 失败（残留锁），新代码通过 |
| 2 | 新增 `forkMutex()` 和 `pthread_atfork(prepare, parent, child)`；`tryLockCore` 在持有 forkMutex 时完成 open、flock、登记槽位；`closeLockFd` 在持有 forkMutex 时取消登记并 close | cross 场景 15（`forkrace`：20 个存活子进程在另一线程反复绑核时被 fork，父进程释放后全部核可用） | 未能稳定复现（竞争窗口太窄），作为回归保护保留 |
| 3 | 排除锁标为 inheritable，不进 fork 槽位，子进程继承 fd 并保持打开，内核层面锁在子进程存活期间一直有效；子进程下次发布时把 ownerPid 改为自己。删除 `acceptedExclusionHolders_` 和 `heldByAcceptedAncestor` | cross 场景 16（`forkexclorphan`：子进程在父进程存活时绑核，父进程随后退出，第三个进程显式预留该排除核） | 复现（`explicit rc=0`），新代码 rc=3 被拒 |
| 4 | `openLockFile` 先不带 `O_CREAT` 打开，ENOENT 时再用 `O_CREAT\|O_EXCL` 创建并 `fchmod 0666`，EEXIST 时重试（最多 8 次） | cross 场景 17（strace 检查打开已有锁文件不带 `O_CREAT`） | 失败，新代码通过 |

验证：tm_infra Release / Debug 4/4；tm_basic Release 9/9；Debug 8/9，失败的 `tsc_clock_test` 是 Debug 未优化下的性能断言（`nowNs()` 比 `system_clock::now()` 便宜），与本改动无关；TSan（`-O1 -fsanitize=thread`）两个测试程序 ALL PASSED，报告目录为空。
