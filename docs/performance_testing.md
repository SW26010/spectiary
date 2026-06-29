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
- `present`: DX11 swap chain Present 完成时间，用于衡量可见帧节奏。
- `display_environment`: 当前窗口所在 monitor、Windows display mode 频率、DWM timing、swapchain refresh desc 和 `Present` sync interval。

主要判定指标：

- `win32 drag move interval`: Win32 拖拽输入到达间隔。用于判断输入源是否足够密。
- `implot pan sample interval`: ImPlot 拖拽采样帧间隔。用于判断交互是否跟上渲染帧。
- `present interval`: 实际可见帧间隔。
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

`scripts/analyze-profile.ps1` 默认是门禁工具：质量门禁或主指标失败时返回非零。只想观察报告、不让脚本失败时，加 `-ReportOnly`。

## 标准采集

先 build：

```powershell
cmd.exe /d /c "call ""C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"" && cmake --build --preset ninja-msvc-debug"
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

`SPECFORGE_PROFILE=1` 是唯一的 profile 开关。默认日志目录为当前工作目录下的 `logs/`；需要把 profile 写到其他位置时，设置 `SPECFORGE_PROFILE_DIR`。

如果只想采集、不自动分析：

```powershell
powershell -ExecutionPolicy Bypass -File scripts\profile-implot-pan.ps1 -SkipAnalyze
```

如果想采集并只看报告、不让性能 miss 让脚本失败：

```powershell
powershell -ExecutionPolicy Bypass -File scripts\profile-implot-pan.ps1 -ReportOnly
```

手动分析某个日志：

```powershell
powershell -ExecutionPolicy Bypass -File scripts\analyze-profile.ps1 logs\specforge-profile-YYYYMMDD-HHMMSS.jsonl
```

手动只看报告：

```powershell
powershell -ExecutionPolicy Bypass -File scripts\analyze-profile.ps1 logs\specforge-profile-YYYYMMDD-HHMMSS.jsonl -ReportOnly
```

144Hz stretch 预算：

```powershell
powershell -ExecutionPolicy Bypass -File scripts\analyze-profile.ps1 logs\specforge-profile-YYYYMMDD-HHMMSS.jsonl -BudgetMs 6.9444
```

120Hz 预算：

```powershell
powershell -ExecutionPolicy Bypass -File scripts\analyze-profile.ps1 logs\specforge-profile-YYYYMMDD-HHMMSS.jsonl -BudgetMs 8.3333
```

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
