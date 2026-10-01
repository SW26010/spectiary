# Pan/drag 历史基线

> 历史记录：本页保留 2026-06-21 和 2026-07-17 的采样与对应实现。时间、硬件、构建和预算属于各次记录，不构成当前版本的性能保证。
> 新采样流程见[主图 pan/drag 测试](../../testing/performance/pan.md)；现行呈现合同和硬件验证范围见[呈现策略](../../presentation/policy.md)。

## 2026-06-21：真实数据 pan/drag 基线

截至 2026-06-21，`logs/spectiary-profile-20260621-064415.jsonl` 来自真实 `.npy` 数据
`carbon_net_increment_loglam_V0.31_X.npy` 的主图 pan/drag 采集。该日志在 130Hz 预算
`BudgetMs 7.6923` 下通过，在 144Hz stretch 预算 `BudgetMs 6.9444` 下失败。

144Hz 失败项是 `win32 drag move interval p95=7.099 ms`、`implot pan sample interval p95=7.268 ms`
和 `present interval p95=7.115 ms`；`input -> present p95=6.940 ms` 和
`view_update duration p95=5.039 ms` 仍满足 144Hz 预算。因此该次采样可以声明该真实数据 pan/drag
场景通过 130Hz 验收，不能声明 144Hz stretch 已达标。


## 2026-07-17：DRR boost 路径与验证

> 本节记录 commit `3aa0680140e9` 的已实现路径和历史 profile 结果，不代表最终产品呈现合同。
> 该路径的 `ALLOW_TEARING` 已由真实 pan 的稳定水平断层证明存在可见撕裂，因此只能作为
> “保持最高刷新率、允许撕裂”的第一层降级，不能作为首选状态。后续方案、降级顺序、
> 可观测性字段和待补硬件矩阵以
> [显示呈现产品合同与验证矩阵](../../presentation/policy.md)为准。

Windows 11 DRR 模式下，主图左键 pan/drag 与命中主图的 Precision Touchpad manipulation
会请求 compositor clock boost。交互期间由独立 compositor-clock waiter 把真实 clock tick
投递给 UI 主循环。DXGI 支持 variable-refresh presentation 时，主 swap chain 和 detached
viewport swap chain 使用 capability-gated `ALLOW_TEARING` flip-model 路径；主窗口按
`Present(0)` 提交，避免再次等待被虚拟化的 DXGI vblank。输入消息只积累下一帧状态，不能
绕过 compositor tick 触发额外渲染。交互结束后释放 boost 并恢复主窗口 `Present(1)`。
waiter 只投递合并后的唤醒消息，不在后台线程访问 ImGui、ImPlot 或 D3D11 对象。clock
pacing 活跃时不并行安排 timer frame；只有 API 不可用、boost 失败或 waiter 退出后，
触控板连续更新才使用 9ms fallback deadline。

验证时必须同时满足：

1. `compositor_clock` 初始化事件为 `available=true`。
2. 交互开始/结束分别出现 `requested=true/false`，开始事件为 `active=true`。
3. DRR 环境下交互窗口的 `present interval p50` 接近 8.33ms，而非 16.67ms。
4. `input -> present p95` 按同一份日志通过目标预算。
5. 交互结束事件为 `requested=false`、`active=false`、结果为 `S_OK`；下一次 boost 前
   `tick_count` 不应增长。若要证明系统最终回到基础刷新率，还应在释放后采集 DWM timing，
   或确认下一次启动的 `display_environment` 已回到基础频率。

`active=true` 不是系统实际升频的充分证据；面板、驱动、供电策略或系统设置仍可能阻止
DRR 升档。Windows 10 或 API 缺失时会自动保留原有 display-vsync 路径，不能把这种降级
记录为 DRR 失败。最小复测矩阵仍应包含 DRR/固定 120Hz × 鼠标/触控板，并用同一份真实
数据、同一交互窗口和同一统计口径比较。

### 2026-07-17 本机 DRR 验证

真实数据 `carbon_net_increment_loglam_V0.31_X.npy` 上：

- 鼠标日志 `logs/spectiary-profile-20260717-184143.jsonl`：DWM 120.000Hz，Present interval
  p50/p95 为 8.317/9.140ms，input→Present p95 为 8.397ms；相对实现前 DRR 的
  16.881ms 改善约 50.3%，与固定 120Hz 的 8.417ms 基本一致。
- 触控板日志 `logs/spectiary-profile-20260717-184604.jsonl`：两个 boost 窗口的 clock
  为 119.966/119.868Hz，合并 Present interval p50/p95 为 8.324/9.322ms，原生输入时间
  →Present p95 为 9.254ms；相对实现前 DRR 的 16.983ms 改善约 45.5%。
- 两次触控板释放均为 `S_OK` 且 `active=false`；释放间隔内 `tick_count` 保持 1860，
  最终释放后 14.404s 才退出且无 `shutdown_release`。该次启动的 DWM timing 为
  60.017Hz，也证明上一轮已最终回落到基础频率。

这些结果应使用 `BudgetMs 8.3333` 解读；脚本默认的 7.6923ms 是独立的 130Hz 验收线。
严格的 120Hz `input→Present p95` 门禁仍未完全通过：鼠标超出 0.064ms，触控板超出
0.921ms。因此当前结论限定为 clock 与 p50 提交节奏达到约 120Hz、可见代理延迟接近减半，
而不是严格 p95 门禁已通过。Present 完成事件也不是直接的 scan-out/光学延迟测量。
