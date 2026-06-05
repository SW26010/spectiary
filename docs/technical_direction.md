# SpecForge 技术方向

## 原则

- 使用官方推荐 backend 和库能力，不自造基础设施。
- 优先使用 ImGui/ImPlot 原生交互。
- 只在被真实 profile 证明后才引入更复杂的渲染或数据路径。
- 代码结构保持普通、可读、可维护，避免过早抽象。

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
- wavelength range: 使用 overview mini plot 或 explicit axis range controls。
- flux range: 默认 auto-fit，提供 lock 和 numeric min/max；必要时再加 overview 或 drag range。
- spectral lines: 使用 vertical lines、annotations、shaded regions 和 legend/selection。
- 大数据: 先测量，再决定 stride/downsample/cache；不要预先写 GPU custom renderer。

如果点数规模接近 ImPlot 高密度风险区，先确认默认 DX11 renderer 是否支持 large mesh/vtx offset，再考虑 32-bit ImDrawIdx 或数据抽样策略。

## 数据边界

当前不决定最终 loader。下一步只需要定义输入合同：

- 数据源路径。
- spectrum count。
- 当前 spectrum index。
- wavelength vector。
- flux vector。
- metadata。
- overlay catalog 输入。

Python、IPC、native loader 或外部预处理都只能作为实现选择，不写入第一阶段产品承诺。

## Profile 事件

JSONL profile 至少区分：

- runtime config。
- pointer press/move/release。
- wheel input。
- range-control input。
- view transform update。
- draw submission。
- render pass duration。
- present timing。
- spectrum switch。
- overlay update。

性能判断规则：

- 120Hz 是常用交互路径目标。
- 144Hz 是 stretch target。
- 没有真实数据和新日志，不声明达标。

## 工程边界

建议后续代码结构：

- `platform`: Win32 window、message loop、DPI、shutdown。
- `renderer`: D3D11 device、swap chain、render target、resize。
- `ui`: ImGui context、style、dockspace、panel orchestration。
- `plot`: ImPlot spectrum view 和 overlay rendering。
- `domain`: spectrum state、view state、selection、catalog state。
- `profile`: JSONL event sink 和 timing helpers。

不要把 spectrum parsing、catalog rules、ImGui widget state 和 rendering device lifecycle 混在同一个文件里。
