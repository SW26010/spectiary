# 显示呈现

本目录维护显示刷新、呈现后端和窗口缩放的合同与诊断资料。产品要求、当前实现和历史证据分别从以下入口阅读。

| 要查什么 | 入口 |
| --- | --- |
| 刷新率、无撕裂、降级顺序、现行实现与硬件验证范围 | [呈现策略](policy.md) |
| SDR 色彩、输入/调度、字体与状态栏帧率的现行平台合同 | [平台呈现合同](platform_contract.md) |
| JSONL 事件、字段、关联规则和 resize 采集/分析方法 | [呈现 telemetry](telemetry.md) |
| 独立面板与主窗口缩放的现状、维护边界、实验和证据 | [窗口缩放调查](live-resize/README.md) |
| DXGI、custom-duration、Composition 与触控板调度的历史采样 | [呈现策略历史证据](evidence/presentation-history.md) |
| 主图 pan、导航、加载与资源测试 | [性能测试入口](../testing/performance_testing.md) |

现行合同在 policy 中维护；窗口缩放的当前决定在 live-resize/README 中维护。design、experiments 和日期 evidence 保留当时的推理及结果，页首指向现行入口。获得新证据时更新对应现行结论，不把旧记录改写成后来发生的事实。

跨模块的长期决策仍放在 [ADR](../adr/)，例如 [detached panel 的 Win32 ownership](../adr/0005-detached-panel-win32-ownership.md)。业务/UI 热路径约束见 [UI 响应速度](../development/ui_responsiveness.md)。
