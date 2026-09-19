# 性能测试入口与公共标准

性能验证按用户动作选择流程。实现层面的热路径约束见 [UI 响应速度](../development/ui_responsiveness.md)，显示刷新与降级要求见[呈现策略](../presentation/policy.md)。

## 选择测试场景

| 场景 | 流程 |
| --- | --- |
| Spectrum 主图左键 pan/drag、Default / Uncapped A/B | [主图 pan/drag](performance/pan.md) |
| 显式添加或打开数据源 | [数据源打开延迟](performance/source-opening.md) |
| Previous/Next、键盘导航、resident/prefetch 相关验证 | [样本导航延迟](performance/navigation.md) |
| 真实进程循环、内存/句柄/GPU 采样和稳定性门禁 | [运行期资源稳定性](performance/resource-stability.md) |
| 原生窗口和 detached viewport resize | [呈现 telemetry](../presentation/telemetry.md)及[窗口缩放调查](../presentation/live-resize/README.md) |

## 公共证据要求

- 对当前改动的结论来自当前构建的新日志；报告保留可执行文件/构建标识、真实 source 标识、显示与窗口设置、预算和目标动作。A/B 保持数据、交互、窗口与显示条件一致，仅改变待比较因素。比较运行期开关时使用同一构建；比较实现改动时保留两侧构建身份，并说明构建差异。Default / Uncapped 的同构建要求见对应流程。
- Synthetic fixture 可验证交互链路；真实数据性能结论必须用实际 .npy、CSV 或 FITS 数据采集。
- JSONL 必须可完整解析，最后是唯一的 profile_recorder_summary，停止原因受支持且 dropped_events 为零。各流程还可能要求完整 operation、lifetime、捕获边界或工作负载周期。
- 分析器是门禁工具。失败退出不能用体感、旧日志或 API 成功替代；缺失数据不能当作零延迟/零丢帧。ReportOnly 仅用于观察报告，历史格式兼容不证明录制完整性。
- Present 完成、应用提交 FPS、实际显示反馈与光学延迟属于不同证据。未测量的显示、CPU、GPU 或功耗指标明确写为不可用或未测量。
- 单元测试、widget 测试和隐藏窗口测试各有验证范围，不能替代真实进程交互、视觉无撕裂或性能预算验收。
- 所有示例命令均从仓库根目录运行。Ninja/MSVC 配置与构建统一使用[工程环境说明](../development/engineering_setup.md)中的 wrapper，并设置超时。

## 目标

主图场景定义见 [pan/drag 的目标](performance/pan.md#目标)。

## 指标口径

帧节奏、输入延迟、120/130/144Hz 预算和主图质量门禁见 [pan/drag 指标口径](performance/pan.md#指标口径)。数据源打开、导航和资源流程各自定义独立门禁，不能套用主图预算。

## 标准采集

主图采集和 Default / Uncapped A/B 命令见[标准采集](performance/pan.md#标准采集)。

### Default / Uncapped A/B

模式设置、Release 配对要求和结果解释见 [Default / Uncapped A/B](performance/pan.md#default--uncapped-ab)。

## 运行期资源稳定性

完整流程见[运行期资源稳定性](performance/resource-stability.md)。

## 显式添加/打开数据源延迟

完整流程见[数据源打开延迟](performance/source-opening.md)。

## 上一条/下一条导航延迟

完整流程见[样本导航延迟](performance/navigation.md)。

## DRR boost 验证

旧 DXGI 路径与 2026-07-17 采样保留在[DRR 历史证据](../evidence/performance/pan-baselines.md#2026-07-17drr-boost-路径与验证)；当前策略和硬件矩阵以[呈现策略](../presentation/policy.md)为准。

## 报告格式

主图报告字段见[pan/drag 报告格式](performance/pan.md#报告格式)。其他场景使用各自流程的结果产物，报告中注明数据身份、条件、完整性、主指标、门禁结论和证据限制。

<a id="当前真实数据基线"></a>

## 历史基线（2026-06-21）

原章节现作为 [2026-06-21 历史基线](../evidence/performance/pan-baselines.md#2026-06-21真实数据-pandrag-基线)保存，不表示该结果仍是当前构建基线。

## 失败解释规则

主图分段定位见[失败解释规则](performance/pan.md#失败解释规则)。

## 注意

历史复盘集中在[UI 响应速度历史记录](../evidence/performance/ui-responsiveness-history.md)；以其复现方法指导新采样，不外推历史结果。
