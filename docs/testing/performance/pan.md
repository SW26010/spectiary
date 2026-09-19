# 主图 pan/drag 性能测试

[性能测试入口](../performance_testing.md) · [测试导航](../README.md)

## 目标

这套流程专门用于判断主图 ImPlot pan/drag 是否跟手。默认场景是：

- 主窗口处于正常桌面大小。
- 主图窗口为 `Spectrum`。
- 用户在主图 plot 区域内按住左键拖拽平移。
- 不混入 wheel zoom、docking 调整、侧栏操作或窗口 resize。

结论必须来自新生成的 JSONL profile，不用旧日志或体感替代。

实现层面的响应性约束、已发生的退化复盘和后续设计规则记录在
[UI 响应速度实现约束与复盘](../../development/ui_responsiveness.md)。

## 指标口径

采集时记录以下事件：

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
- `compositor_clock`: 初始化与 boost 状态变化。`available` 表示 Windows API 可用，
  `requested` 表示交互策略提出请求，`active` 表示请求成功且 compositor tick pacing 正在运行；
  `imgui_drag_active` 表示 ImGui 捕获的左键拖动正在请求低延迟 UI 呈现
  （uncapped ImPlot pan 除外）。
- `runtime_config` / `pan_pacing`: 分别记录会话请求和实际生效的 pan pacing，以及 pan 活跃时的
  backend、Present mode、sync interval、flags、tearing 支持和连续渲染状态。

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
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\build-ninja-msvc-debug.ps1 -TimeoutSec 180
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

程序启动后只做一件事：在 `Spectrum` 主图 plot 区域按住左键连续平移 10-15 秒，然后关闭程序。脚本会等待 Spectiary 退出，再分析本次运行生成的 `logs/spectiary-profile-*.jsonl`。

### Default / Uncapped A/B

`profile-implot-pan.ps1` 的 `-PanPacing` 只接受 `Default` 或 `Uncapped`。`Default` 不设置
`SPECTIARY_PAN_PACING`，保留正常 Composition/compositor-clock 行为；`Uncapped` 只为本次子进程
设置精确的 `SPECTIARY_PAN_PACING=uncapped`，让主窗口 `Spectrum` pan 使用 DXGI immediate
提交。脚本退出时会恢复调用者原有的环境变量。

正式 A/B 必须使用同一个新构建的 Release 可执行文件、同一真实数据、同一窗口尺寸和显示模式。
先构建一次：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass `
  -File scripts\build-ninja-msvc-debug.ps1 `
  -Configure `
  -Preset ninja-msvc-release-static `
  -Target spectiary_native `
  -TimeoutSec 1200

powershell -NoProfile -ExecutionPolicy Bypass `
  -File scripts\build-ninja-msvc-debug.ps1 `
  -Preset ninja-msvc-release-static `
  -Target spectiary_native `
  -TimeoutSec 1200
```

第一条配置 preset，第二条构建同一 preset 的 `spectiary_native`。然后分别运行：

```powershell
$exe = ".\build\ninja-msvc-release-static\Spectiary.exe"
$source = "C:\path\to\same-real-source.npy"

powershell -NoProfile -ExecutionPolicy Bypass `
  -File scripts\profile-implot-pan.ps1 `
  -Executable $exe `
  -InitialSource $source `
  -PanPacing Default `
  -BudgetMs 8.3333

powershell -NoProfile -ExecutionPolicy Bypass `
  -File scripts\profile-implot-pan.ps1 `
  -Executable $exe `
  -InitialSource $source `
  -PanPacing Uncapped `
  -BudgetMs 8.3333
```

两组各自在 `Spectrum` 主图连续左键 pan 至少 10-15 秒；不要在一次窗口内混入 resize、docking、
wheel zoom 或侧栏操作。记录 source 路径或内容标识，因为进程启动时异步 snapshot 可能尚未进入
`runtime_config.source`，仅凭该字段为空不能自证两组使用了同一数据。日志仍是运行产物，不纳入提交。

launcher 会把选择的模式作为 `-ExpectedPanPacing` 传给 analyzer。手动分析时必须显式给出期望模式：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass `
  -File scripts\analyze-profile.ps1 `
  logs\spectiary-profile-<default-timestamp>.jsonl `
  -ExpectedPanPacing Default `
  -BudgetMs 8.3333

powershell -NoProfile -ExecutionPolicy Bypass `
  -File scripts\analyze-profile.ps1 `
  logs\spectiary-profile-<uncapped-timestamp>.jsonl `
  -ExpectedPanPacing Uncapped `
  -BudgetMs 8.3333
```

`-ExpectedPanPacing Uncapped` 除了检查 requested/effective marker，还要求 pan 活跃 marker 自证
`backend=dxgi`、`present_mode=immediate`、`sync_interval=0`、与 tearing 支持一致的 flags，以及
`continuous_rendering=true`。marker 缺失或不一致时分析失败；不能把一个普通 profile 错标为
Uncapped 结果。

Uncapped 报告中的 `submission_fps` 只表示带 recorder 插桩时测得的应用提交上限，不是实际显示
帧率或光子到屏幕速率。DXGI 没有本流程使用的 Composition feedback，analyzer 会把实际显示反馈写成
`unavailable`，不能解释成零丢帧。CPU、GPU 和功耗没有外部计数器时同样保持 `unmeasured`。

本诊断模式只支持主窗口中的 `Spectrum` pan。detached ImGui viewport 不切换到 uncapped Present
合同，也不属于本次结果保证范围；正式 A/B 应把 `Spectrum` 停靠在主窗口。detached viewport 的
Present 仍可能影响整个 UI frame，不能把这种混合场景与主窗口结果合并。

自动化采集继续使用 `SPECTIARY_PROFILE=1`，以便从进程启动阶段保留完整上下文。Release 版本也可以通过
`Settings > Diagnostics` 在运行时开始/停止采集；沉浸模式右上角的 `REC` 标记表示正在录制。复现卡顿后
尽快停止录制，分析时结合停止前的一段帧时间线和输入事件定位。Portable build 的默认输出目录是可执行
文件旁的 `logs/`，用户可在 Diagnostics 设置中修改；性能脚本会显式设置
`SPECTIARY_PROFILE_DIR`，覆盖 UI 设置并把本次分析日志重定向到仓库 `logs/`，避免和 portable 包内状态
混在一起。

默认文件名保留 `spectiary-profile-` 前缀和本地时间戳，并追加进程 ID 与进程内序列号；文件以
create-new 语义分配，因而同一时刻的普通实例不会互相截断。重启时遇到未完成的旧 JSONL 会分配新文件，
分析器仍以末尾的 `profile_recorder_summary` 判断录制是否完整；已有的 `spectiary-profile-*.jsonl` 日志
和按通配符收集的分析流程继续有效。

运行时录制使用 4 MiB 有界队列和后台批量写入，不在输入/UI 热路径同步写磁盘。单次录制达到 5 分钟或
100 MiB 时自动停止。producer/writer 的普通内存锁争用不会丢事件；只有队列确实达到 4 MiB 容量时才
拒绝新事件，并在末尾的 `profile_recorder_summary` 中记录 `dropped_events`。设置页停止只请求后台 drain，
不会在 UI 帧同步等待文件 flush。显式停止以及时长/大小自动边界都会先关闭普通录制；若真实 render
frame 已经在途，则只保留该帧的 duration、成功 Present、`navigation_latency` 和
`source_load_latency` 尾部事件，等 Present 处理完才封口并由后台 writer 写 summary，因此同帧完成的
延迟报告不会被 stop 丢弃。没有 render frame
在途时（包括窗口 minimized/hidden），自动边界立即封口并 drain，不依赖未来恢复窗口。自动停止和 writer
完成都会唤醒事件驱动 UI 以刷新 `REC` 状态。
用于定量回归时必须由 analyzer 确认 summary 完整且 `dropped_events == 0`。

## 其他采集和手动分析

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
powershell -ExecutionPolicy Bypass -File scripts\analyze-profile.ps1 logs\spectiary-profile-YYYYMMDD-HHMMSS-mmm.jsonl
```

手动只看报告：

```powershell
powershell -ExecutionPolicy Bypass -File scripts\analyze-profile.ps1 logs\spectiary-profile-YYYYMMDD-HHMMSS-mmm.jsonl -ReportOnly
```

144Hz stretch 预算：

```powershell
powershell -ExecutionPolicy Bypass -File scripts\analyze-profile.ps1 logs\spectiary-profile-YYYYMMDD-HHMMSS-mmm.jsonl -BudgetMs 6.9444
```

120Hz 预算：

```powershell
powershell -ExecutionPolicy Bypass -File scripts\analyze-profile.ps1 logs\spectiary-profile-YYYYMMDD-HHMMSS-mmm.jsonl -BudgetMs 8.3333
```

## 报告格式

报告结论按这个顺序写：

1. 数据源：日志文件名、真实 source 标识、窗口/显示器刷新率、预算。
2. 场景：只包含 ImPlot 主图左键 pan/drag，还是混入了其他操作。
3. 实际环境：列出 requested/effective pan pacing、backend、Windows mode Hz、DWM Hz、DWM period、
   Present mode、sync interval、flags 和 tearing 支持。
4. 结果：列出提交 FPS、axis-update FPS、重复提交比例，以及 `win32 drag move interval`、
   `input -> axis limits`、`input -> present`、`present interval`、`implot pan sample interval`
   和 instrumented frame-work 的 p50/p95/p99。
5. 诊断：如果失败，说明失败发生在输入到达、ImPlot 采样、render pass 还是 Present。
6. 结论：使用脚本末尾 `Result: PASS/FAIL`；Uncapped 只声明应用提交上限，并明确实际显示 feedback
   是否可用，不外推到显示帧率、detached viewport、CPU/GPU/功耗或其他交互。

## 失败解释规则

- `win32 drag move interval` 慢：输入事件本身没有足够高频，先查系统、鼠标、窗口消息路径。
- `win32 drag move interval` 快，但 `implot pan sample interval` 慢：输入到了，但 ImGui/ImPlot 或主循环没有每帧采样。
- `implot pan sample interval` 快，但 `input -> present` 慢：交互更新到了，瓶颈更可能在 render pass、Present 或帧节奏。
- `view_update` 慢：UI/plot 逻辑成本高，优先看点数、overlay、状态更新和 ImPlot 调用。
- `render_pass` 或 `present` 慢：优先看 DX11 swap chain、vsync、窗口状态和 GPU/显示器路径。

## 注意

当前 shell 同时保留 synthetic fixture 和真实数据 loader。Synthetic fixture 可以验证 shell 和 ImPlot 交互链路，但不能证明真实光谱数据达标。真实性能结论必须用实际 `.npy`、CSV 或 FITS 数据源按同一流程重新生成日志。

## 历史基线

[2026-06-21 pan 基线与 2026-07-17 DRR 验证](../../evidence/performance/pan-baselines.md)保留当时的日志、预算和结论，不替代当前构建的新采样。
