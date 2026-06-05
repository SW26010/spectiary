# SpecForge 产品需求

## 当前阶段

仓库已经完成第一次项目整理、native shell 和第一条真实数据竖切片。当前可提交状态包括：

- `specforge_native` Windows executable target。
- Win32 + DirectX 11 + Dear ImGui docking + ImPlot shell。
- 主图通过 `SpectrumSnapshotHandle` 消费 domain 快照；synthetic fixture 和 `.npy` loader 使用同一条 UI/plot 路径。
- `.npy` loader 支持 1D 或行级 2D float32/float64 array，并一次渲染一条光谱。
- 可选 JSONL profile sink 已经接入，用于解释输入、view update、draw、render/present 的基本链路。

当前仍不声明真实数据性能达标；任何刷新率或延迟结论必须来自真实数据和新日志。

## 产品目标

SpecForge 是一个本地 Windows 光谱查看工具。目标体验是优雅、跟手、克制、专业，用户可以长时间反复检查真实光谱数据，而不会被布局噪音、延迟或不稳定交互打断。

目标技术栈：

- Win32
- DirectX 11
- Dear ImGui docking branch
- ImPlot
- CMake
- vcpkg

## 核心用户与工作流

核心用户是在本地桌面环境检查碳星、SDSS、LAMOST 或类似数据的使用者。优先级最高的是直接操作延迟、图形稳定性和信息密度。

核心工作流：

1. 打开真实光谱数据源。
2. 查看当前光谱，X 轴为 wavelength，Y 轴为 flux。
3. 在主图中拖拽平移。
4. 使用鼠标滚轮围绕光标缩放。
5. 使用范围导航控制 wavelength window。
6. 控制 flux window，支持手动固定和恢复自适应。
7. 在矩阵数据中切换上一条和下一条光谱。
8. 查看谱线、谱带和分组 overlay。
9. 使用窗口预设。
10. 输出 profile 日志，解释输入、状态更新、绘制和帧节奏。

## 必须保留的需求

- 主图是第一视觉层级。
- 子窗口必须支持自由 docking。
- 所有用户可见文案使用领域语言，不暴露 backend 名称。
- profiling 是产品能力，不是可选开发工具。
- 真实性能结论必须来自真实数据和新日志。
- 旧实验只能作为参考，不作为新产品主线。

## 对交接稿的取舍

### 接受

- ImGui/ImPlot 是新的产品主线。
- 旧 Qt、VisPy、OpenGL 或 Python texture 路径只能做行为参考和性能对照。
- 真实数据、可比较 profile 日志、pan、zoom、range navigation、spectrum switch、overlay 都属于产品验收。
- 144Hz 只能作为 stretch target，不能用单一指标或旧日志宣称达标。

### 调整

- 交接稿中关于 Python 继续承担数据与业务状态的描述不能直接搬到本仓库。SpecForge 的目标栈是 native Windows C++，当前已通过 `SpectrumSnapshotHandle` 和 `.npy` loader 落地第一条输入合同；后续 Python、导出文件、IPC 或 native loader 只能作为 producer 侧实现选择，不能渗入 UI/plot 路径。
- range slider 不要求照搬旧 UI。优先用 ImPlot 的轴限制、overview plot、drag rect、drag line、numeric inputs 或 lock toggles 组合出更适合 ImGui 的交互。
- 谱线 overlay 不需要手写 canvas 系统。优先使用 ImPlot 的 line、annotation、shaded region 和 legend/selection 能力。
- pan 和 wheel zoom 优先使用 ImPlot 的交互和 axis limits。只有证明确实不能满足光谱工作流时，才引入自定义 transform 层。
- default layout 可以用 ImGui dockspace 建立，布局持久化交给 ImGui ini。DockBuilder 若用于初始布局，必须被隔离为一次性初始化逻辑。

### 不接受

- 不做自研 docking 系统。
- 不做泛化多 backend 框架。
- 不移植每一个历史实验。
- 不在第一阶段引入复杂 Python bridge、GPU custom plot renderer 或专用 profiling analyzer，除非有清晰验收需求。
- 不把 demo 视觉效果、装饰性面板或虚构业务模块带进产品提交。

## 里程碑

### Phase 0: Clean House

状态：已完成，作为仓库卫生和路线边界的基线。

范围：

- 整理 README、需求、技术方向和环境文档。
- 清理生成产物、本地数据和旧 demo 提交路径。
- 声明 Win32/DX11/ImGui/ImPlot 目标依赖。
- 不在该阶段实现产品代码。

验收：

- `build/`、`vcpkg_installed/`、本地数据和 scratch 实验被忽略。
- vcpkg manifest 指向目标技术栈。
- 文档解释清楚下一步实现边界。

### Milestone 1: Native Shell

状态：已完成初始 native shell。

范围：

- 增加 Win32 + DirectX 11 application shell。
- 初始化 Dear ImGui docking 和 ImPlot。
- 建立 dock host、主图窗口、侧栏、状态栏和 profile sink。
- 保留 synthetic fixture 作为 shell smoke data，不从它得出真实性能结论。

验收：

- 窗口启动稳定。
- docking 可用，子窗口可拆分、停靠和恢复布局。
- DX11 resize、render target 和 shutdown 路径清楚。

### Milestone 2: Spectrum Vertical Slice

状态：第一条 `.npy` 真实数据竖切片已存在，后续继续收敛交互和 profile 验收。

范围：

- 接入第一种真实光谱输入：1D 或行级 2D `.npy` float32/float64 array。
- 显示一条光谱，并支持在 2D matrix 中切换上一条和下一条光谱。
- 支持主图 pan 和 cursor-centered wheel zoom。
- 输出 input、view update、draw、present timing 的 JSONL profile。

验收：

- `.npy` 真实数据通过和 synthetic fixture 相同的 snapshot/UI/plot 路径打开并渲染。
- pan、zoom 和 spectrum previous/next 可用。
- profile 日志能解释基本延迟指标；真实性能目标仍必须用真实数据和新日志单独证明。

### Milestone 3: Product Interaction

范围：

- wavelength 和 flux range navigation。
- spectrum previous/next 的工作流收敛。
- view preset。
- 范围夹取和无效窗口防护。

验收：

- 用户可以完成日常检查工作流。
- 新日志可与旧参考行为做 A/B 对照。

### Milestone 4: Overlay And Readiness

范围：

- public/hidden spectral line catalog。
- line、band、group overlay。
- overlay selection 和 profile attribution。
- 窄窗口和常见桌面窗口布局验证。

验收：

- overlay 开启后主图仍保持可读和可操作。
- profile 能归因 overlay update。

## 非目标

- 当前阶段不做 FITS loader 或泛化多 loader 框架。
- `.npy` loader 是第一条真实数据竖切片，不等于最终数据栈承诺。
- 当前阶段不做性能目标证明。
- 当前阶段不引入本地大数据。
- 当前阶段不创建旧项目兼容层。
