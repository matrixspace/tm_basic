# TscClock 保证读取时间非递减

- 任务时间：2026-10-29
- 涉及文件：
  - `tm_kit/basic/real_time_clock/TscClock.hpp`
  - `src/real_time_clock/TscClock.cpp`
  - `test/tsc_clock_test/TscClockTest.cpp`

## 1. 需求

`TscClock::nowNs()` 返回的时间必须**非递减**：任何一次调用返回到的值，不得小于此前任意一次调用已经返回到的值。

改动前的行为（见原头文件注释）是：

- 同一线程内，正常的周期重同步（slew）不会倒退；
- 但當 `system_clock` 自身被回拨超过 `stepThreshold`（默认 1 ms）时，`resync()` 会"跟随"系统时间，把映射锚点直接设到新的系统时间，于是时钟会**向后跳变**；
- 退化路径（无 invariant TSC / 非 x86-64 / `forceSystemClock`）直接返回 `system_clock::now()`，系统时间被回拨时同样会倒退。

因此需要去掉这两处例外。

## 2. 方案

### 2.1 TSC 路径：重锚定时钳制，永不后退

映射是 `ns = baseNs + (tsc - baseTsc) * mult / 2^32`。每次 `resync()` 都会计算当前映射在采样 tick 上的值 `cur`，再与系统时间 `sysNs` 比较：

- 向前跳变（`err > stepThreshold`）：跟随系统时间，锚定到 `sysNs`（向前跳，不破坏单调性）；
- 向后跳变（`err < -stepThreshold`）：**不跟随**，锚定到 `cur`，时钟保持当前速率继续走，直到系统时间追上；
- 采样本身无法扩展速率基线（`tsc <= calTsc_` / `sysNs <= calNs_`）：同样按上面的方向处理。

实现上统一为：

```cpp
anchor(tsc, std::max(cur, sysNs), lastKnownRate);
```

即新锚点值永远不小于旧映射在该 tick 上的值。由于旧映射在 `tsc` 之前的所有 tick 上取值都 `<= cur`，新映射在 `tsc` 之后取值都 `>= cur`，所以跨重同步时映射是连续的，读者（包括因为 seqlock 重试而换了映射的读者）不会看到更小的值。

向后跳变时仍会把 `calTsc_/calNs_` 重置到本次采样点，避免回拨污染后续的长基线速率估计；`ticksPerNs_` 保持不变（invariant TSC 速率稳定）。在这段时间内 `stats().steps` 与 `lastErrorNs` 会持续反映这个未跟随的偏差。

### 2.2 退化路径：高水位钳制

`forceSystemClock` / 无 invariant TSC 时，`nowNs()` 走 `fallbackNowNs()`：

- 用实例内的 `std::atomic<int64_t> lastFallbackNs_` 保存已返回过的最大值；
- 每次用 CAS 循环把 `system_clock` 读数抬升到该高水位（`std::max` 语义），返回值不低于高水位。

因此系统时间被回拨时，退化路径返回"已返回过的最大值"，系统时间追上后自动恢复返回系统时间。代价是退化路径每次读取多一次原子操作（TSC 热路径不受影响）。

### 2.3 兼容性

- 公开 API 未变（`Options`、`Stats`、`nowNs()`、`now()` 签名均不变）。
- 语义变化：系统时间被回拨后，时钟不再跟随，而是保持在前方（这是本需求的要求）。
- 新增一个仅测试使用的 `friend struct TscClockTestAccess;` 前置声明，运行时零开销。

## 3. 测试

测试程序：`test/tsc_clock_test/TscClockTest.cpp`（`tsc_clock_test` 目标）。

新增/调整的用例：

| 用例 | 内容 |
|---|---|
| `testMonotonicAcrossSteps` | 把 `stepThreshold` 设为 1 ns、`resyncInterval` 设为 1 ms，使几乎每次重同步都走 step 分支（系统时间在映射两侧来回），2 线程高频读取 1 s，断言没有任何一次倒退，且 step 分支被充分执行（`steps >= 100`）。 |
| `testBackwardErrorDoesNotStepBack` | **确定性**用例。通过 `TscClockTestAccess` 把映射人为抬到系统时间前方 100 µs，让读者先观察到该值；再把锚点整体后移（不改变任何 tick 的映射值）使下一次读取立即重同步。重同步看到约 -100 µs 的误差（远大于 10 µs 的 `stepThreshold`），断言 `steps` 恰好加一、`lastErrorNs <= -50 µs`，且新读数不小于旧读数。 |
| `testFallback`（扩展） | 4 线程并发读取退化路径 1 s 不倒退；并通过测试访问把高水位设到未来 1 ms，断言读数不低于该高水位、后续读数继续非递减。 |

### 回归验证

把实现临时改回旧行为（`anchor(tsc, sysNs, ...)`）后重新编译运行：

```
      backward error -100003ns at re-sync, reading delta -98505ns
FAIL: a backward system_clock error does not move the clock back
1 FAILURE(S)
```

恢复修复后：

```
      backward error -99998ns at re-sync, reading delta 1263ns
ok:   a backward system_clock error does not move the clock back
...
ALL PASSED
```

### 实测数据（release，连续运行 3 次均通过）

- 精度（对 system_clock，3 s，约 750 万样本）：p99 = 0 ns，max = 0 ns（本机时钟源一致）；29 次重同步，0 次 step。
- step 压力用例：1 s 内 999 次重同步、998 次 step，0 次倒退。
- 单次调用成本（吞吐循环）：TscClock ≈ 16 ns，`system_clock::now()` ≈ 29 ns，热路径未受影响。

## 4. 多核/多线程适用范围（实测）

测试机：2×Intel Xeon Gold 5117（2 socket × 14 核 × 2 SMT = 56 逻辑核），TSC 2.0 GHz；内核当前 clocksource 为 `tsc`，dmesg 无 TSC 同步失败告警。

| 场景 | 结论 | 依据 |
|---|---|---|
| 同一线程（含线程迁移） | 可以保证 | 重同步钳制 + seqlock；连续 `rdtsc` 对 2 亿次 0 次倒序；线程迁移耗时 µs 级，远大于核间偏差 |
| 同一 socket 的多线程 | 可以保证 | 同 socket 两核双向握手的最小延迟完全相同（差 0~2 cycles），TSC 偏差在测量噪声内为 0 |
| 跨 socket 的多线程 | 严格意义取决于平台 TSC 是否跨 socket 同步 | `invariant TSC` CPUID 位只保证恒定速率/不停机，不保证跨 socket 同步；本机跨 socket 双向最小延迟差 6~61 cycles（3~30 ns），交换角色后符号不稳定，量级为几十 ns |
| 退化路径（forceSystemClock / 无 invariant TSC） | 可以保证，且全局线性一致 | 实例内原子高水位 CAS，与核/socket/系统时间回拨无关 |

跨 socket 补充实测：带真实通信延迟的 happens-before 测试（CPU0↔1、0↔3、2↔1，各 60 万次交换）0 次违例；只有两个线程的读取间隔小于 TSC 偏差（~几十 ns）时才可能出现顺序不一致。

**注意 RDTSC 保序：** 未加 LFENCE 的同一测试在跨 socket 上会出现 38~1897 次"违例"，那是 `RDTSC` 非序列化导致读者把时钟读提前到"观察到对端值"之前，并非时钟倒退；加 LFENCE 后消失。若上层代码在紧耦合握手后立即取时间戳并比较，需要自行保序。

**若需要与拓扑无关的严格跨线程单调：** 可给 TSC 路径也加"每次读取一次全局原子高水位"。代价是多核竞争下的原子 RMW：实测退化路径 4 线程约 700 万次/秒，而 TSC 路径单线程约 6000 万次/秒，因此未作为默认行为。

LFENCE 代价实测（3 个 map 原子 load + rdtsc 的微基准）：plain 11.5 ns，尾部 LFENCE 15.8 ns，头部 17.2 ns，前后各一 21.9 ns，RDTSCP 17.1 ns；完整 `nowNs()` 由 15.8 ns 升到 29.5 ns（前后各一），会抵消 TSC 路径的优势，故未默认加入。

上述适用范围之外的限制、以及本次明确"保持现状"的已知问题，统一记录在第 5 节。

## 5. 已知问题与限制

**处理决定：保持现状** —— 不默认加 LFENCE，不提供"严格全局单调"选项。以下问题作为已知限制记录，供后续使用和演进参考。

### 5.1 跨 socket 的严格全局单调依赖平台 TSC 同步

- **触发条件**：两个线程分布在不同 socket，且两次读取的间隔小于核间 TSC 偏差（本机实测为几十 ns 量级）。
- **影响**：跨线程时间戳顺序可能出现该偏差量级的不一致；同一线程、同一 socket 不受影响。
- **现状**：`invariant TSC` CPUID 位只保证恒定速率/不停机，不保证跨 socket 同步；类信任平台 TSC。本机内核 clocksource 为 `tsc` 且无同步失败告警，实际使用中未观察到违例（60 万次跨 socket 握手 0 次）。
- **规避**：对顺序敏感的多线程放到同一 socket；或改用退化路径（全局原子高水位）。
- **备选修复**：构造时做跨核 TSC 同步检查并自动回退；或增加 `strictGlobalMonotonic` 选项（每次读取一个全局原子，多核争用下代价高）。

### 5.2 RDTSC 不是序列化指令

- **触发条件**：指令乱序窗口内恰好发生重同步；或上层代码在紧耦合跨线程握手后立即取时间戳并比较。
- **影响**：
  - `nowNs()` 内部：tick 理论上可能被重排到 seqlock 检查之外，从而用旧映射搭配新 tick；在 `anchor(max(cur, sysNs))` 钳制下，最坏情况只是速率变化带来的极小偏差（默认 slew ≤ 1000 ppm），概率极低。
  - 上层调用：取时间戳可能被重排到"观察到对端事件"之前，得到比对端更早的时间戳，表现为跨线程顺序上的轻微"倒退"。实测未加 LFENCE 的握手测试在跨 socket 上出现 38~1897 次/60 万次，加 LFENCE 后为 0。
- **现状**：保持现状、性能优先，`nowNs()` 内未加 LFENCE。
- **规避**：上层在紧耦合跨线程比较时自行保证 RDTSC 保序（取时间戳前后加 LFENCE），或通过带序的消息传递建立先后关系。
- **备选修复**：`nowNs()` 内加 LFENCE（见第 4 节代价数据：完整调用 15.8 → 29.5 ns）或改用 RDTSCP（约 17.1 ns 微基准）。

### 5.3 invariant TSC 检测只读 CPUID 位

- `hasInvariantTsc()` 只检查 CPUID `0x80000007` EDX bit 8，不校验跨核同步，也不识别虚拟机/固件环境中"该位置位但 TSC 被缩放或按 vCPU 偏移"的情况。
- **规避**：虚拟化或对跨核顺序敏感的场景使用 `Options::forceSystemClock`，退化路径仍然保证非递减。

### 5.4 系统时间回拨后时钟会长期偏前

- **行为**：向后调整被忽略，时钟保持当前速率继续走，直到系统时间追上。若系统时间被手工回拨 1 小时，则之后 1 小时内本时钟比 `system_clock`/墙上时间快 1 小时。
- **影响**：与外部系统时间比较的绝对值会有偏差；单调性不受影响。这是本需求的预期行为，但使用方需要知道"此钟不再等价于墙上时间"。
- **观测**：这段时间内 `stats().lastErrorNs` 持续为负，`stats().steps` 每次重同步都会增加（见 5.5）。

### 5.5 `stats().steps` 的语义

- `steps` 统计的是"走 step 分支的次数"，不是"系统时钟跳变次数"。系统时间被回拨并被忽略期间，每次周期重同步都会走 step 分支并 +1（默认约 10 次/秒），直到系统时间追上。
- 依赖 `steps` 做告警/诊断的代码需要注意这一点；`lastErrorNs` 可直接反映当前偏差。

### 5.6 退化路径的原子高水位有竞争开销

- 退化路径每次读取都要做一次全局 CAS；4 线程争用实测约 700 万次/秒，而 TSC 路径单线程约 6000 万次/秒。
- 仅影响 `forceSystemClock` / 无 invariant TSC 的退化路径，TSC 热路径不受影响。

### 5.7 不同 `TscClock` 实例之间不协调

- 每个实例有独立的映射和退化高水位；跨实例比较时间戳（尤其是系统时间回拨后）不保证单调。
- **规避**：同一条时间线使用同一个实例（`TscClockComponent` 内部持有一个共享的 `TscClock`）。

### 5.8 `maxSlewPpm >= 1e6` 会破坏单调性

- slew 分支的速率为 `(1/tpn) * (1 + slew)`，`slew ∈ [-maxSlewPpm*1e-6, +maxSlewPpm*1e-6]`；当 `maxSlewPpm >= 1e6` 时该因子可能为 0 或负，映射在两次重同步之间不再递增。
- **现状**：默认 1000 ppm 安全；构造时没有校验或钳制。
- **备选修复**：在构造函数中钳制该参数或断言。

### 5.9 极小的 `stepThreshold` 会让时钟单向"棘轮"

- 把 `stepThreshold` 设到与采样噪声同量级（如 1 ns）时，向前的误差会被跟随、向后的误差被忽略，时钟会逐步向前累积偏差（测试中 1 s 内 `lastErrorNs` 约 -2~-10 µs）。
- 这是新策略的直接推论，仅在病态配置下明显；默认 1 ms 不受影响。测试 `testMonotonicAcrossSteps` 正是用这种配置压测 step 分支。

## 6. 关联文档

- 代码说明：`tm_kit/basic/real_time_clock/TscClock.hpp`
- 历史任务文档（工作区根目录，非本仓库）：`docs/TscClock_explanation.html`（其中"除非 system_clock 跳变"的描述已被本次改动取代）
