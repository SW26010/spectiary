# SpecForge 响应速度测试流程

## 目标

这套流程专门用于判断主图 ImPlot pan/drag 是否跟手。默认场景是：

- 主窗口处于正常桌面大小。
- 主图窗口为 `Spectrum`。
- 用户在主图 plot 区域内按住左键拖拽平移。
- 不混入 wheel zoom、docking 调整、侧栏操作或窗口 resize。

结论必须来自新生成的 JSONL profile，不用旧日志或体感替代。

实现层面的响应性约束、已发生的退化复盘和后续设计规则记录在
[UI 响应速度实现约束与复盘](ui_responsiveness.md)。

## 指标口径

采集时同时记录四类事件：

- `input`: Win32 mouse input。拖拽期间重点看 `pointer_move`，并记录 x/y 和按键状态。
- `implot.pan_drag.state`: ImPlot 主图 pan-drag 开始和结束。
- `implot.pan_drag.sample`: pan-drag 活跃期间每帧一次的 ImPlot plot 坐标和 axis limits。
- `implot.axis_limits_changed`: ImPlot axis limits 变化，证明 view transform 已经更新。
- `touchpad.gesture`: Precision Touchpad 原生手势增量，记录 `input_steady_ns`、`kind`、
  pan/zoom 数值和 inertia；事件自身的 `steady_ns` 是 plot 消费该增量的时间。
  `input_steady_ns` 是 Direct Manipulation `OnContentUpdated` 产出有效 transform delta 的
  时间，不是触控板硬件或原始 pointer packet 时间，因此可测应用消费延迟，但不能单独
  证明设备到应用的完整输入延迟。
- `present`: DX11 swap chain Present 调用完成时间；clock pacing 活跃时可作为提交帧节奏代理，
  但不是光子到达屏幕的直接测量。
- `display_environment`: 当前窗口所在 monitor、Windows display mode 频率、DWM timing、swapchain refresh desc 和 `Present` sync interval。
- `compositor_clock`: 初始化与 boost 状态变化。`available` 表示 Windows API 可用，`requested` 表示交互策略提出请求，`active` 表示请求成功且 compositor tick pacing 正在运行。

主要判定指标：

- `win32 drag move interval`: Win32 拖拽输入到达间隔。用于判断输入源是否足够密。
- `implot pan sample interval`: ImPlot 拖拽采样帧间隔。用于判断交互是否跟上渲染帧。
- `present interval`: Present 完成间隔；与 compositor tick 对齐时用于判断提交是否跟上目标刷新率。
- `input -> pan sample`: 输入到下一次 ImPlot pan-drag frame sample。
- `input -> axis limits`: 输入到下一次 axis limits 变化。
- `input -> present`: 输入到下一次 Present 完成。
- `view_update`、`draw_submission`、`render_pass`、`present` duration: 用于区分 UI 逻辑、draw submission、GPU render pass 和 Present 阻塞。

默认预算：

- 120Hz: p95 <= 8.3333 ms。
- 130Hz acceptance: p95 <= 7.6923 ms。
- 144Hz stretch: p95 <= 6.9444 ms。

一次结果只有在 `win32 drag move interval`、`implot pan sample interval`、`present interval`、`input -> axis limits`、`input -> present` 都满足同一预算时，才能说该 pan/drag 场景达到了对应刷新率目标。

默认质量门禁：

- `ImPlot pan-drag window time >= 10000 ms`。
- 左键拖拽 `pointer_move` 样本数 `>= 100`。
- 必须存在 `implot.pan_drag.sample`、`implot.axis_limits_changed` 和 `present` 事件。
- JSONL 必须可完整解析，且最后一条事件是唯一的 `profile_recorder_summary`。
- summary 必须给出受支持的停止原因，且 `dropped_events == 0`。

`scripts/analyze-profile.ps1` 默认是门禁工具：录制完整性、质量门禁或主指标失败时返回非零。只想观察报告、
不让脚本失败时，加 `-ReportOnly`。没有 summary 的历史日志默认失败；只有明确分析旧格式时才加
`-AllowLegacyIncompleteRecording`，脚本会显示 legacy 警告，且无法证明录制完整性。截断或非法 JSONL
即使在 legacy 模式下也不会被接受。

## 标准采集

先 build：

```powershell
cmd.exe /d /c "call ""C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"" && cmake --build --preset ninja-msvc-portable-debug"
```

运行 130Hz 验收采集：

```powershell
powershell -ExecutionPolicy Bypass -File scripts\profile-implot-pan.ps1
```

用真实数据采集时直接传入初始 source：

```powershell
powershell -ExecutionPolicy Bypass -File scripts\profile-implot-pan.ps1 -InitialSource "C:\path\to\source.npy"
```

运行 144Hz stretch 采集时显式传入 6.9444 ms 预算：

```powershell
powershell -ExecutionPolicy Bypass -File scripts\profile-implot-pan.ps1 -BudgetMs 6.9444 -InitialSource "C:\path\to\source.npy"
```

程序启动后只做一件事：在 `Spectrum` 主图 plot 区域按住左键连续平移 10-15 秒，然后关闭程序。脚本会等待 SpecForge 退出，再分析本次运行生成的 `logs/specforge-profile-*.jsonl`。

自动化采集继续使用 `SPECFORGE_PROFILE=1`，以便从进程启动阶段保留完整上下文。Release 版本也可以通过
工具栏的 `Performance > Start Recording` / `Stop Recording` 在运行时开始/停止采集；沉浸模式右上角的
`REC` 标记表示正在录制。复现卡顿后尽快停止录制，分析时结合停止前的一段帧时间线和输入事件定位。
Portable build 不设置 `SPECFORGE_PROFILE_DIR` 时默认写入可执行文件旁的
`Data/logs/`；性能脚本会显式设置 `SPECFORGE_PROFILE_DIR`，把本次分析日志重定向到仓库 `logs/`，避免和
portable 包内状态混在一起。

运行时录制使用 4 MiB 有界队列和后台批量写入，不在输入/UI 热路径同步写磁盘。单次录制达到 5 分钟或
100 MiB 时自动停止。producer/writer 的普通内存锁争用不会丢事件；只有队列确实达到 4 MiB 容量时才
拒绝新事件，并在末尾的 `profile_recorder_summary` 中记录 `dropped_events`。菜单停止只请求后台 drain，
不会在 UI 帧同步等待文件 flush；自动停止和 writer 完成都会唤醒事件驱动渲染以刷新 `REC` 状态。
用于定量回归时必须由 analyzer 确认 summary 完整且 `dropped_events == 0`。

如果只想采集、不自动分析：

```powershell
powershell -ExecutionPolicy Bypass -File scripts\profile-implot-pan.ps1 -SkipAnalyze
```

触控板手势使用相同的真实数据启动和 JSONL 采集链路，但当前自动门禁仍只针对左键 pan。采集触控板时使用 `-SkipAnalyze`，在 plot 内连续双指平移或捏合 10–15 秒；检查 `touchpad.gesture`、随后发生的 `implot.axis_limits_changed` 和 `present`。不要把鼠标门禁脚本的 PASS 外推为触控板延迟结论。

如果想采集并只看报告、不让性能 miss 让脚本失败：

```powershell
powershell -ExecutionPolicy Bypass -File scripts\profile-implot-pan.ps1 -ReportOnly
```

手动分析某个日志：

```powershell
powershell -ExecutionPolicy Bypass -File scripts\analyze-profile.ps1 logs\specforge-profile-YYYYMMDD-HHMMSS-mmm.jsonl
```

手动只看报告：

```powershell
powershell -ExecutionPolicy Bypass -File scripts\analyze-profile.ps1 logs\specforge-profile-YYYYMMDD-HHMMSS-mmm.jsonl -ReportOnly
```

144Hz stretch 预算：

```powershell
powershell -ExecutionPolicy Bypass -File scripts\analyze-profile.ps1 logs\specforge-profile-YYYYMMDD-HHMMSS-mmm.jsonl -BudgetMs 6.9444
```

120Hz 预算：

```powershell
powershell -ExecutionPolicy Bypass -File scripts\analyze-profile.ps1 logs\specforge-profile-YYYYMMDD-HHMMSS-mmm.jsonl -BudgetMs 8.3333
```

## DRR boost 验证

> 本节记录 commit `8282a95` 的已实现路径和历史 profile 结果，不代表最终产品呈现合同。
> 该路径的 `ALLOW_TEARING` 已由真实 pan 的稳定水平断层证明存在可见撕裂，因此只能作为
> “保持最高刷新率、允许撕裂”的第一层降级，不能作为首选状态。后续方案、降级顺序、
> 可观测性字段和待补硬件矩阵以
> [显示呈现产品合同与验证矩阵](presentation_policy.md)为准。

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

- 鼠标日志 `logs/specforge-profile-20260717-184143.jsonl`：DWM 120.000Hz，Present interval
  p50/p95 为 8.317/9.140ms，input→Present p95 为 8.397ms；相对实现前 DRR 的
  16.881ms 改善约 50.3%，与固定 120Hz 的 8.417ms 基本一致。
- 触控板日志 `logs/specforge-profile-20260717-184604.jsonl`：两个 boost 窗口的 clock
  为 119.966/119.868Hz，合并 Present interval p50/p95 为 8.324/9.322ms，原生输入时间
  →Present p95 为 9.254ms；相对实现前 DRR 的 16.983ms 改善约 45.5%。
- 两次触控板释放均为 `S_OK` 且 `active=false`；释放间隔内 `tick_count` 保持 1860，
  最终释放后 14.404s 才退出且无 `shutdown_release`。该次启动的 DWM timing 为
  60.017Hz，也证明上一轮已最终回落到基础频率。

这些结果应使用 `BudgetMs 8.3333` 解读；脚本默认的 7.6923ms 是独立的 130Hz 验收线。
严格的 120Hz `input→Present p95` 门禁仍未完全通过：鼠标超出 0.064ms，触控板超出
0.921ms。因此当前结论限定为 clock 与 p50 提交节奏达到约 120Hz、可见代理延迟接近减半，
而不是严格 p95 门禁已通过。Present 完成事件也不是直接的 scan-out/光学延迟测量。

## 报告格式

报告结论按这个顺序写：

1. 数据源：日志文件名、窗口/显示器刷新率、预算。
2. 场景：只包含 ImPlot 主图左键 pan/drag，还是混入了其他操作。
3. 实际环境：列出 `display_environment` 中的 Windows mode Hz、DWM Hz、DWM period 和 `Present` sync interval。
4. 结果：列出 `win32 drag move interval p95`、`input -> axis limits p95`、`input -> present p95`、`present interval p95`、`implot pan sample interval p95`。
5. 诊断：如果失败，说明失败发生在输入到达、ImPlot 采样、render pass 还是 Present。
6. 结论：使用脚本末尾 `Result: PASS/FAIL`，只声明该日志证明的刷新率目标，不外推到真实数据或其他交互。

## 当前真实数据基线

截至 2026-06-21，`logs/specforge-profile-20260621-064415.jsonl` 来自真实 `.npy` 数据
`carbon_net_increment_loglam_V0.31_X.npy` 的主图 pan/drag 采集。该日志在 130Hz 预算
`BudgetMs 7.6923` 下通过，在 144Hz stretch 预算 `BudgetMs 6.9444` 下失败。

144Hz 失败项是 `win32 drag move interval p95=7.099 ms`、`implot pan sample interval p95=7.268 ms`
和 `present interval p95=7.115 ms`；`input -> present p95=6.940 ms` 和
`view_update duration p95=5.039 ms` 仍满足 144Hz 预算。因此当前可以声明该真实数据 pan/drag
场景通过 130Hz 验收，不能声明 144Hz stretch 已达标。

## 失败解释规则

- `win32 drag move interval` 慢：输入事件本身没有足够高频，先查系统、鼠标、窗口消息路径。
- `win32 drag move interval` 快，但 `implot pan sample interval` 慢：输入到了，但 ImGui/ImPlot 或主循环没有每帧采样。
- `implot pan sample interval` 快，但 `input -> present` 慢：交互更新到了，瓶颈更可能在 render pass、Present 或帧节奏。
- `view_update` 慢：UI/plot 逻辑成本高，优先看点数、overlay、状态更新和 ImPlot 调用。
- `render_pass` 或 `present` 慢：优先看 DX11 swap chain、vsync、窗口状态和 GPU/显示器路径。

## 注意

当前 shell 同时保留 synthetic fixture 和真实数据 loader。Synthetic fixture 可以验证 shell 和 ImPlot 交互链路，但不能证明真实光谱数据达标。真实性能结论必须用实际 `.npy`、CSV 或 FITS 数据源按同一流程重新生成日志。
