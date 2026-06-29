# SpecForge 产品需求

## 当前阶段

仓库已经完成第一次项目整理、native shell 和多格式真实数据 loader。当前可提交状态包括：

- `specforge_native` Windows executable target。
- Win32 + DirectX 11 + Dear ImGui docking + ImPlot shell。
- 主图通过 `SpectrumSnapshotHandle` 消费 domain 快照；synthetic fixture、`.npy`、CSV 和 FITS loader 使用同一条 UI/plot 路径。
- loader 支持 1D/2D `.npy`、简单波长/流量 `.csv`、可识别的单条 LAMOST/SDSS FITS table 光谱。
- 已有第一版公开谱线表 `config/spectral_lines.public.tsv`，由 Spectral Lines 面板搜索和组织，并在主图中显示 line/band 参考 overlay；公开表不包含 subtype 组合、窗口预设或私有判据。
- 可选 JSONL profile sink 已经接入，用于解释输入、view update、draw、render/present 的基本链路。

当前最新真实数据 profile 只证明主图 pan/drag 在 130Hz 预算下通过；144Hz 仍是未达标的 stretch target。任何刷新率或延迟结论必须来自真实数据和新日志。

## 产品目标

SpecForge 是一个本地 Windows 光谱查看工具。目标体验是优雅、跟手、克制、专业，用户可以长时间反复检查真实光谱数据，而不会被布局噪音、延迟或不稳定交互打断。

UI 跟手程度是产品基础能力，不是可以被其它功能换取的优化项。任何 loader、overlay、sample filtering、labeling、面板状态或本地状态功能，都不能让主图 pan、wheel zoom、上一条/下一条、range navigation 等实时交互退化；需要重计算、IO 或状态构造时，必须隔离、缓存、延后，或用真实数据 profile 证明不影响交互预算。

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
- 实时交互响应优先于附加功能；功能验收必须证明没有牺牲主图 pan、zoom、spectrum switch、range navigation 的跟手程度。
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

状态：多格式真实数据输入已存在，后续继续收敛交互和 profile 验收。

范围：

- 接入真实光谱输入：1D/2D `.npy`、简单 `.csv`、可识别的单条 FITS table 光谱。
- 显示一条光谱，并支持在 2D `.npy` matrix 或多行 vector-table FITS 中切换上一条和下一条光谱。
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

状态：已完成第一版公开线表、UI 搜索/组织和主图 reference overlay；hidden/local overlay、窗口预设和 profile attribution 仍待后续收敛。

范围：

- public/hidden spectral line catalog。
- line、band、group overlay。
- overlay selection 和 profile attribution。
- 窄窗口和常见桌面窗口布局验证。

验收：

- overlay 开启后主图仍保持可读和可操作。
- profile 能归因 overlay update。

### Milestone 5: Sample Annotation And Labeling

目标：样本标注是下一阶段的核心产品能力，但必须按 sample navigation、read-only sample annotation inspection、editable sample labeling、sample filtering 的顺序推进，避免一次性把完整标注系统压进当前查看器主线。

范围：

- 建立 sample navigation 的独立 surface，支持 row index 和 sample name 定位。
- 自动加载同前缀 `*_y.npy` 作为只读 sample annotation result。
- 在当前样本上显示已加载 annotation value 或映射后的 sample label。
- 保持 manual labeling、sample filtering、外部输出 autosave 和 relink 为后续子阶段。

验收：

- annotation 数据必须与 source collection 的 spectrum count 严格匹配，不匹配时不附着到当前 source。
- 只读 annotation 显示不改变原始数据，也不隐式创建 editable sample labeling task。
- sample navigation 与主图切换保持一致，且不把 labeling window 作为当前样本索引的所有者。
- 第一条竖切片完成后，再根据真实工作流验证决定进入可编辑 labeling。

## 非目标

- 当前阶段不做 catalog 管理、旧项目兼容层或泛化多后端框架。
- 当前 loader 支持 `.npy`、简单 `.csv` 和可识别的单条 LAMOST/SDSS FITS table 光谱；受限 image fallback 不等于可靠 image FITS 或最终数据栈承诺。
- 当前阶段不做性能目标证明。
- 当前阶段不引入本地大数据。
- 当前阶段不创建旧项目兼容层。
