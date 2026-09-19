# 2026-09-12：增量 buffer 有界生命周期回归

> 历史记录：2026-09-12 起的采样、判断及本页注明的后续补充。文中状态属于记录时间；当前决定与后续安排见[窗口缩放调查入口](../README.md)。

状态：自动化回归通过；2026-09-16 人工窗口行为检查反馈无异常。完整应用资源观察尚未完成，不据此启用生产策略或关闭 #56。

## 工作负载与结果

在现有 `specforge_sdr_swap_chain_tests` 中新增 `TestIncrementalBufferRepeatedLifetimes`，不增加运行时机制或独立测试框架。使用普通样式的两个隐藏 HWND，共用真实 D3D11 device/context，各自持有 Composition presentation 对象。

- 24 轮创建、销毁；每轮两个窗口各提交 8 次尺寸变化，共 384 次 acquire / Present 成功，无 DXGI 回退。
- 尺寸在 320×240、640×360、1280×720、480×800、960×540 之间振荡，两窗口使用不同顺序。
- 每次验证真实 render texture 与最新请求尺寸一致，并检查另一窗口的 allocation generation 不受影响。
- 每轮 Shutdown 后检查应用持有的 texture、RTV、presentation buffer、available/statistics/surface handle 及 selected/bound slot 清空。
- 不可用 buffer 仅在测试中有限重试；没有改变应用等待或 presentation policy。没有物理显示完成断言。
- Composition 不可用时显式输出 SKIP；本机本次没有 SKIP。

构建使用仓库 MSVC wrapper；5 项 CTest 全部通过：swap-chain、layout persistence、presentation trace、viewport policy architecture、profile schema。新增负载所在测试耗时 5.87 秒，总计 7.90 秒。原有四阶段分配故障、预算规划及 DXGI 回退用例也通过。

原始输出：`logs/incremental-stability-20260912.txt`。对应基底为 `d1f822a` 加本次测试及文档工作区修改；产品源码未改动。

## 实际资源观察及限制

每轮 teardown 后等待 100 ms，使用系统接口采样。数值属于该测试进程，不是完整 SpecForge 应用；缓存和异步释放可能继续变化。

| 指标 | 第 1 轮 | 第 9 轮 | 第 24 轮 |
| --- | ---: | ---: | ---: |
| 进程句柄数 | 252 | 252 | 252 |
| Private bytes | 31,899,648 | 34,643,968 | 34,951,168 |
| GPU local CurrentUsage（bytes） | 1,155,072 | 1,798,144 | 1,822,720 |
| GPU non-local CurrentUsage（bytes） | 0 | 0 | 0 |

句柄数稳定，内存增长明显减缓，但数值并未全部恢复到第一轮水平。因此结论是短时负载未发现句柄累积或按整套纹理规模持续增长；**不是无泄漏证明**，也没有用 256 MiB 逻辑预算冒充实测峰值。本次仅测 teardown 后趋势，不覆盖活动期间的实际 GPU 峰值、长时间使用或可见 DWM 工作负载。

接口语义：[QueryVideoMemoryInfo](https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_4/nf-dxgi1_4-idxgiadapter3-queryvideomemoryinfo) 提供当前进程的 adapter segment 用量；[GetProcessHandleCount](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-getprocesshandlecount) 提供进程句柄数。计数不等于独占 buffer 所有权；所有权清空通过单独断言检查。

## 生产候选维护审查

按 ADR 0010，候选仍限于现有 Composition buffer 的选择和替换，复用原有 Resize、BeginFrame、Present、Shutdown 及 DXGI fallback。当前没有必要扩张到渲染线程或窗口系统重构。

- 继续保留有界 planner、一次替换、失败时旧 slot 不变以及既有 fallback 约束。
- 预算是每 viewport 的逻辑纹理预算，多个窗口累计资源仍需完整应用观察。
- 生产启用时单独评审支持范围、默认策略和失败路径；现有实验开关不构成发布批准。
- 无正向体验收益的 no-redirection / feedback-acquire-only 不应随候选启用；实验 executable 和生成 backend 仍与标准程序隔离。
- 本次只新增测试与文档，没有改变 resize、presentation、主窗口调度或 ImGui ownership policy。

## 可见窗口回归及反馈

2026-09-16，用户针对下面的检查步骤反馈“未发现异常”。按用户反馈记录人工窗口行为回归通过，不再要求重复同一检查。本次没有提供新日志路径、构建 hash 或资源测量，因此不将其表述为新的性能样本、逐项自动验证或完整应用无泄漏证明。下面保留当时的操作步骤用于追溯。

无需再重复相同 A/B。使用普通样式 A + IncrementalBuffers 一次有限交互检查（最多 240 秒）：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/profile-live-resize.ps1 -Scenario NativeSize -RedirectionArm A -FeedbackBreakdown -IncrementalBuffers -TimeoutSec 240
```

先保持 recording 关闭，完成下面动作：

1. 分离 Spectrum 和另一个面板，交替缩放两个外边框约 30 秒，确认内容持续更新、无黑屏和尺寸错位。
2. 将面板停靠回主窗口，再分离，重复三次；检查内部 dock 分界线仍流畅。
3. 最小化/恢复主窗口；切换窗口焦点；关闭并重新打开独立面板，确认显示和窗口归属正常。
4. 如需保留最后一次 resize 的性能检查，再在 Diagnostics 开始唯一一次 5 秒录制并立即缩放 Spectrum 外边框；录制结束后正常退出。

反馈日志路径，以及各动作是否出现卡死、黑屏、面板消失或持续恶化。本轮可见交互不能替代完整应用内存趋势采样；如出现资源持续增长，先定位复现条件，不继续累加优化策略。主窗口原生外边框期间不刷新仍由 #101 单独处理。

## 2026-09-16：完整应用资源观察准备

原采集脚本新增可选 `-ObserveResources`，在启动的应用进程外每约 2 秒采样一次，写入同一日志目录的 `resources.csv`。保留 executable hash、PID 和实验配置。此模式默认关闭性能录制，允许无 JSONL 正常结束；没有录制时不会宣称性能校验通过，少于两个资源样本则报错。

记录 elapsed_ms、private/working-set bytes、handle_count，以及 GPU Process Memory 的 dedicated/shared/committed bytes 与实例数。CIM 查询限于本次启动 PID，设置 1 秒操作超时；没有实例、字段缺失或查询失败均保留 `unavailable` 和空值，有错误时附原因，不当作零用量。采样会带来额外开销，此轮不用来评价帧时间。

GPU 数据只是辅助趋势。[微软记录过 GPU Process Memory 计数器误报增长的问题](https://learn.microsoft.com/en-us/troubleshoot/windows-client/performance/gpu-process-memory-counters-report-wrong-value)，且共享资源可能被多个进程计入。它不是 buffer 独占分配量或物理显存峰值，也不等价于之前测试中的 QueryVideoMemoryInfo。可疑增长不能直接定性为泄漏；应与进程数据及已有生命周期证据结合，必要时再用有限的进程内采样核实。

准备工作验证：真实系统接口试读成功，当前无 GPU 工作负载的测试进程得到 unavailable；四种模拟场景覆盖无实例、查询失败、不完整字段和多实例汇总。CTest 的 resource observation、profile schema、tier contract 三项通过。产品 EXE 未重建，resize/presentation policy 未改动。

操作（从仓库根目录运行，不需要点击 Start Recording）：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/profile-live-resize.ps1 -Scenario NativeSize -RedirectionArm A -FeedbackBreakdown -IncrementalBuffers -ObserveResources -TimeoutSec 240
```

1. 加载与前次相同的数据，分离 Spectrum，保持静止 15 秒。
2. 反复缩放外边框 30 秒，恢复到大致相同尺寸，静止 15 秒；重复三轮，保持数据及面板数量不变。
3. 停靠回主窗口，静止 20 秒，正常退出。发回日志目录，并指出有无额外打开文件/面板等改变负载的动作。

此轮回答完整应用资源是否随重复缩放持续增长；不是再次要求证明流畅度或重做已通过的窗口行为检查。分析时比较相同阶段的静止区间，排除首次加载/预热；没有完整观察前不记为资源验收通过，也不自动启用生产策略。
