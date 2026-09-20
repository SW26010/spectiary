# Spectiary 产品需求

本页记录稳定的产品目标、交互要求和范围边界。术语以根目录
[CONTEXT.md](../CONTEXT.md) 为准；数据、标注与谱线的详细合同见
[领域参考](reference/README.md)，技术选型见[技术方向](technical_direction.md)。

实施与验收进度在 [GitHub Issues](https://github.com/SW26010/spectiary/issues) 管理。
早期交接取舍、阶段状态和里程碑保留在[初始路线记录](evidence/product/initial-roadmap.md)。
性能是否达标必须按[测试标准](testing/performance_testing.md)用真实数据和新日志判断，
不能由历史里程碑推断。

## 产品目标

Spectiary 是一个本地 Windows 光谱查看工具。目标体验是优雅、跟手、克制、专业，用户可以长时间反复检查真实光谱数据，而不会被布局噪音、延迟或不稳定交互打断。

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
5. 使用 Windows Precision Touchpad 双指平移、捏合缩放；图内控制双轴，轴区域约束单轴。
6. 使用范围导航控制 wavelength window。
7. 控制 flux window，支持手动固定和恢复自适应。
8. 在矩阵数据中切换上一条和下一条光谱。
9. 查看谱线、谱带和分组 overlay。
10. 使用窗口预设。
11. 输出 profile 日志，解释输入、状态更新、绘制和帧节奏。

## 必须保留的需求

- 主图是第一视觉层级。
- 子窗口必须支持自由 docking。
- `F11` immersive mode 是主图检查用的非 dockable presentation mode，用于临时绕过 dock host 直接全屏呈现绘图区；它不改变普通子窗口必须支持自由 docking 的默认合同。
- 所有用户可见文案使用领域语言，不暴露 backend 名称。
- profiling 是产品能力，不是可选开发工具；Release 必须提供运行时开关、明确录制指示、有界存储和丢事件计数，不能在输入/UI 热路径同步写磁盘。
- 真实性能结论必须来自真实数据和新日志。
- 实时交互响应优先于附加功能；功能验收必须证明没有牺牲主图 pan、zoom、spectrum switch、range navigation 的跟手程度。
- 旧实验只能作为参考，不作为新产品主线。

## 文件源列表与启动恢复

启动时恢复已保存的文件源清单，并以最后保存的激活来源决定显示内容。
后台加载的完成先后不改变这个选择，也不因为某个其它来源加载成功就自动替换它。

| 启动恢复结果 | 主图与样本工作区 | 状态栏 |
| --- | --- | --- |
| 最后激活的来源恢复成功 | 正常显示该来源及当前样本 | 其它来源的恢复失败不触发 `Load failed` |
| 最后激活的来源恢复失败 | 不显示光谱和当前样本内容，不回退到其它已加载来源 | 显示 `Load failed` |
| 最后激活的来源仍在加载 | 不临时显示其它已恢复来源 | 按当前加载状态显示 |

Files 表格中，恢复失败的来源保留为灰色行，文件名、类型和状态单元格不可用于选择该来源。
即使没有可显示的光谱，表格仍展示这些行；鼠标悬停可查看来源路径和失败原因。
移除按钮保持可用，移除后同时忘记该来源的启动恢复记录，下次启动不再自动加载它。
从清单移除来源不删除磁盘上的光谱或标注文件。

保留失败来源是为了允许暂时离线的文件在后续启动时恢复，不把一次加载失败视为用户要求删除来源。
未移除的失败来源继续保留保存的样本位置和标注关联；若它是最后激活的来源，退出后也继续保存该激活选择。
用户切换到正常来源或移除失败的激活来源后，旧的启动失败提示不再占据状态栏。

这里规定的是已有来源的启动恢复。首次打开一个新来源就失败，不会仅因该失败将它加入恢复清单。

回归覆盖：`tests/source_collection_activation_transaction_tests.cpp` 验证激活来源、加载顺序、
连续启动和移除后的持久化；`tests/widget/source_files_widget_tests.cpp` 验证无光谱时的失败行、
禁用选择、悬停提示和可用的移除按钮。

## 外部打开与实例选择

`设置 > 常规 > 文件打开` 提供两个持久化选项：`在新实例中打开`（默认）和
`添加到最近使用的实例并激活`。升级时缺少此设置仍使用新实例。
文件关联、Explorer、快捷方式、拖到 EXE 和等价 CLI 路径参数使用同一规则。
无路径启动仍创建新实例；应用内打开和自动化 `source.open` 不受影响。
Files 的“在新实例打开”始终创建新实例，对应 CLI 的 `--new-instance`。

复用仅限当前启动所解析的配置命名空间：不同 Portable 目录及不同配置根互不参与
候选选择。同一命名空间内选最近使用、协议兼容且仍存活的普通 GUI 实例；没有目标、
目标退出/无响应或无法激活时，在有界等待后由本次启动的新实例打开，不丢弃请求。
实例发现与转发的总等待预算为 1.5 秒，不包含正常启动和数据源加载时间。

已有 source collection 只激活，不重复添加或隐式重新加载，保留当前 sample 和工作状态。
与“将外部打开的光谱文件作为文件夹源打开”组合时，原始目标成员信息完整传递；
指定 FITS/CSV 会成为该文件夹源的当前 spectrum，不默认选择目录中的第一个文件。
若成员已从目录消失、无法加载或被 sample filter 排除，沿既有错误路径反馈。
普通 GUI 实例不共享实时状态，也不复用自动化 named pipe。
技术合同见 [ADR 0016](adr/0016-external-open-instance-routing.md)。

## 交互与实现约束

### 技术与验收边界

- ImGui/ImPlot 是新的产品主线。
- 旧 Qt、VisPy、OpenGL 或 Python texture 路径只能做行为参考和性能对照。
- 真实数据、可比较 profile 日志、pan、zoom、range navigation、spectrum switch、overlay 都属于产品验收。
- 144Hz 只能作为 stretch target，不能用单一指标或旧日志宣称达标。

### 交互与布局要求

- Spectiary 的目标栈是 native Windows C++，当前已通过 `SpectrumSnapshotHandle` 和 `.npy` loader 落地第一条输入合同；后续 Python、导出文件、IPC 或 native loader 只能作为 producer 侧实现选择，不能渗入 UI/plot 路径。
- range slider 不要求照搬旧 UI。优先用 ImPlot 的轴限制、overview plot、drag rect、drag line、numeric inputs 或 lock toggles 组合出更适合 ImGui 的交互。
- 谱线 overlay 不需要手写 canvas 系统。优先使用 ImPlot 的 line、annotation、shaded region 和 legend/selection 能力。
- 鼠标 pan 和 wheel zoom 优先使用 ImPlot 原生交互；Precision Touchpad 使用 Windows Direct Manipulation 采集双指 pan/pinch，再统一映射到 ImPlot axis limits，不引入自定义 renderer。
- 触屏输入暂缓，不属于当前 Precision Touchpad 验收范围。未来实现前先确定一指输入用于直接 pan，还是保留给点击选择/标注，再确定双指 pan/pinch 语义；实现应复用现有 plot axis-limit controller，但为 `PT_TOUCH` 增加独立的多接触点捕获与真实触屏验收。
- Immersive mode 为了 edge-to-edge plot，允许仅在 immersive display option 下使用内绘轴 overlay 和 edge-band wheel zoom；普通 docked plot 仍走 ImPlot 原生交互。
- 当前 immersive mode 使用透明的 ImPlot 原生轴，Y 轴刻度生成和数字格式交给 ImPlot；不得为固定轴区域宽度隐藏小数刻度或强制保留一位小数。刻度标签宽度变化暂时允许改变绘图区的水平位置，稳定布局由 #125 的后续 ImPlot #668 评估处理。
- default layout 可以用 ImGui dockspace 建立，布局持久化交给 ImGui ini。DockBuilder 必须隔离在默认布局初始化与用户显式恢复逻辑中。
- 设置中的“恢复默认布局”与“视图 → 恢复默认布局”共用恢复逻辑：恢复标准停靠布局，显示所有核心面板，并将其收回当前主窗口工作区。仅修改布局和面板可见性，不清除数据源、标注、任务/草稿、标签定义、sample filtering/sorting 或无关偏好；恢复后的布局通过普通 ImGui ini 路径保存。
- 默认布局不显示设置窗口。每次打开设置时，将其解除停靠并按当前主窗口所在显示器工作区居中、限制尺寸，避免旧显示器坐标或分辨率变化导致设置不可达。

### 实现范围限制

- 不做自研 docking 系统。
- 不做泛化多 backend 框架。
- 不移植每一个历史实验。
- 没有清晰验收需求时，不引入复杂 Python bridge、GPU custom plot renderer 或专用 profiling analyzer。
- 不把 demo 视觉效果、装饰性面板或虚构业务模块带进产品提交。

## 非目标

- 当前阶段不做 catalog 管理、旧项目兼容层或泛化多后端框架。
- 当前 loader 的唯一 CFITSIO reader 负责 table/image container，语义层只识别
  受限的 SDSS/LAMOST 与 generic 单光谱结构；能打开 container 不代表能解释为
  光谱，也不构成通用 FITS 或最终数据栈承诺。
- 不用 synthetic fixture 或历史日志替代当前真实性能验收。
- 不把本地大数据作为仓库内容提交。
