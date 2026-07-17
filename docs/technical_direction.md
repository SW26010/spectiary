# SpecForge 技术方向

## 原则

- 使用官方推荐 backend 和库能力，不自造基础设施。
- 优先使用 ImGui/ImPlot 原生交互。
- UI 跟手程度是基础约束。任何 loader、overlay、sample filtering、labeling、panel orchestration 或本地状态功能，都不能以牺牲 pan、zoom、spectrum switch、range navigation 的实时响应为代价。
- 只在被真实 profile 证明后才引入更复杂的渲染或数据路径。
- 代码结构保持普通、可读、可维护，避免过早抽象。

响应性实现约束和已发生退化的复盘见 [UI 响应速度实现约束与复盘](ui_responsiveness.md)。

## 目标栈

- Windowing: Win32
- Renderer: DirectX 11
- UI: Dear ImGui docking branch
- Plotting: ImPlot
- Build: CMake presets
- Dependencies: vcpkg manifest mode

## 官方参考

- Dear ImGui README 说明官方仓库提供平台和渲染 backends，并建议 C++ 项目优先组合标准 backend，例如 Win32 + DX11，而不是重写 backend: <https://github.com/ocornut/imgui>
- Dear ImGui Getting Started 有 Raw Win32 API + DirectX11 示例和初始化顺序: <https://github.com/ocornut/imgui/wiki/Getting-Started>
- Dear ImGui example_win32_directx11 是实现 shell 时的主要代码参考: <https://github.com/ocornut/imgui/blob/master/examples/example_win32_directx11/main.cpp>
- vcpkg 的 `imgui` port 提供 `docking-experimental`、`win32-binding` 和 `dx11-binding` features: <https://vcpkg.io/en/package/imgui.html>
- ImPlot README 提醒高密度绘图需要关注 16-bit index 限制、renderer vtx offset 或 32-bit indices，并说明 ImPlot 适合实时交互绘图: <https://github.com/epezent/implot>
- Windows Precision Touchpad 对未启用原生手势的桌面程序通常回退为 wheel 消息；主图的双指平移和捏合缩放使用 Windows Direct Manipulation，并只接管命中 plot 的 `PT_TOUCHPAD`: <https://learn.microsoft.com/en-us/windows/win32/input-precisiontouchpad/precision-touchpad-portal>、<https://learn.microsoft.com/en-us/windows/win32/directmanipulation/direct-manipulation-portal>
- Windows 11 DRR 的高刷新交互使用官方 compositor clock API 请求 boost，并通过 compositor clock tick 驱动帧节奏；不能只请求 boost 后继续依赖被虚拟化的 DXGI vblank。DXGI 报告支持时，boosted flip-model swap chain 同时使用 capability-gated variable-refresh/tearing flags，普通帧仍保持同步提交: <https://learn.microsoft.com/en-us/windows/win32/directcomp/compositor-clock/compositor-clock>、<https://learn.microsoft.com/en-us/windows/win32/direct3ddxgi/variable-refresh-rate-displays>
- CMake Presets 文档区分可提交的 `CMakePresets.json` 和本地的 `CMakeUserPresets.json`: <https://cmake.org/cmake/help/latest/manual/cmake-presets.7.html>

## Docking 方向

必须使用 Dear ImGui docking branch。默认结构：

- 一个全窗口 dock host。
- 主图、数据浏览、overlay、range controls、profile、console 等都是普通 ImGui windows。
- 所有产品面板都可 dock、undock、re-dock。
- 不实现自研 docking。
- 布局持久化优先交给 ImGui ini。
- 多 viewport 可以在 shell 稳定后启用，但必须验证 DPI、窗口恢复、focus 和 monitor 切换。

DockBuilder 只允许用于初始布局种子。如果使用，必须隔离在小函数里，并在文档中说明它依赖 docking branch 的实验 API。

## ImPlot 使用方向

优先使用 ImPlot 提供的能力：

- main plot: `ImPlot::BeginPlot` 和 `ImPlot::PlotLine`。
- pan/zoom: 使用 ImPlot axis interaction 和 axis limits。
- touchpad: 平台层用 Direct Manipulation 产出平移/缩放增量，plot 层只负责统一的 axis-limit 变换；普通、沉浸和 detached viewport 共用这条路径，不复制模式专用控制器。
- touchscreen（暂缓）: 当前 Win32 输入源只接管 `PT_TOUCHPAD`，不能把触屏兼容鼠标消息视为触屏支持。未来应将输入源泛化为共享的 Direct Manipulation gesture source，通过 `WM_POINTERDOWN` / `PT_TOUCH` 和逐接触点 `SetContact` 处理多指输入，同时复用现有命中区域、手势增量和 axis-limit controller；实现前必须先确定一指 pan 与点击选择/标注的优先级，并用真实触屏验证多指中心、capture、兼容鼠标去重和性能。参考 <https://learn.microsoft.com/en-us/windows/win32/api/directmanipulation/nf-directmanipulation-idirectmanipulationviewport-setcontact>。
- wavelength range: 使用 overview mini plot 或 explicit axis range controls。
- flux range: 默认 auto-fit，提供 lock 和 numeric min/max；必要时再加 overview 或 drag range。
- spectral lines: 使用 vertical lines、annotations、shaded regions 和 legend/selection。
- 大数据: 先测量，再决定 stride/downsample/cache；不要预先写 GPU custom renderer。

如果点数规模接近 ImPlot 高密度风险区，先确认默认 DX11 renderer 是否支持 large mesh/vtx offset，再考虑 32-bit ImDrawIdx 或数据抽样策略。

## 数据边界

当前真实数据 loader 支持 `.npy`、简单波长/流量 `.csv`，以及可识别的单条 LAMOST/SDSS FITS table 光谱。真实数据输入合同定义在
[Spectrum Snapshot Contract](spectrum_snapshot_contract.md)，由 `domain` 产出
稳定快照，UI 和 plot 只读该快照。

Python、IPC、native loader 或外部预处理都只能作为 producer 侧实现选择，不能写入 UI/plot 产品承诺。

当前 FITS reader 只能作为窄口径 vertical slice。若后续 FITS 支持继续扩张，应先抽到独立 `fits_spectrum_loader` 边界，并优先评估 CFITSIO/CCfits，而不是继续在通用 loader 文件里叠加手写 FITS 细节。

## Profile 事件

JSONL profile 至少区分：

- runtime config。
- pointer press/move/release。
- wheel input。
- touchpad gesture input（包含原生输入时间、pan/zoom 类型和 inertia 标记）。
- range-control input。
- view transform update。
- draw submission。
- render pass duration。
- present timing。
- compositor clock 初始化、boost 请求/释放、可用性和 HRESULT。
- spectrum switch。
- overlay update。

性能判断规则：

- 120Hz 是常用交互路径目标。
- 130Hz 是当前真实数据 pan/drag 验收线。
- 144Hz 是 stretch target。
- 没有真实数据和新日志，不声明达标。
- `compositor_clock.active=true` 只证明应用已成功请求并启动 clock pacing；是否实际升至 120Hz 仍以交互窗口内的 input/present interval 为准。
- 触碰每帧 UI、plot、overlay、sample filtering、labeling 或 source/session view 构造的功能改动，必须用真实数据 profile 证明交互预算未退化；不能只用单元测试或 synthetic fixture 代替。

## 工程边界

建议后续代码结构：

- `platform`: Win32 window、message loop、DPI、shutdown。
- `platform/win32_compositor_clock`: Windows 11 API 动态发现、成对 boost 生命周期和 clock tick 唤醒；UI 主循环将普通输入无效化合并到下一次 tick，renderer 只负责 capability-gated DXGI present flags，不把 DRR API 细节扩散到 plot/UI。
- `renderer`: D3D11 device、swap chain、render target、resize。
- `ui`: ImGui context、style、dockspace、panel orchestration。
- `plot`: ImPlot spectrum view 和 overlay rendering。
- `domain`: spectrum state、view state、selection、catalog state。
- `profile`: JSONL event sink 和 timing helpers。

不要把 spectrum parsing、catalog rules、ImGui widget state 和 rendering device lifecycle 混在同一个文件里。
