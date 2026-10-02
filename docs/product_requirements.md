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

同一 runtime/config namespace 内，没有兼容普通 GUI 实例时，无来源参数的普通启动按下述规则恢复。
已有兼容实例时，再次无来源参数启动恢复完整 Files 清单，但不设置启动激活目标，没有当前光谱或样本。
后台来源恢复只登记到 Files，不先激活旧来源再清空；用户可点击已恢复的来源进行激活。
未显式改变来源就关闭时，不把原有持久化激活来源覆盖为空；显式打开、选择或删除来源后进入正常持久化流程。
应用设置、语言、主题、面板和窗口布局仍正常恢复及保存。自动化与资源测试启动不受此策略影响。
显式文件/文件夹启动仍恢复完整 Files 清单，并由显式来源覆盖激活目标；外部打开路由规则不变。
Windows 创建的任务栏 Shift-click、Win+Shift+数字或其它再次启动都走同一策略，应用不检测按键或 shell 来源。

启动时恢复已保存的文件源清单，并以最后保存的激活来源决定显示内容。
后台加载的完成先后不改变这个选择，也不因为某个其它来源加载成功就自动替换它。
通用 JSON 资源限制内的全部合法来源项都应恢复，不按固定来源数量静默截断。
若 cache 含非法来源项，跳过时给出诊断，并按原始项映射激活来源；激活项无效时不误选相邻来源。

source-session cache v3 为标注关联保存 source-only identity，不使用包含标注文件状态的
context fingerprint。恢复时身份缺失（包括 v1/v2 cache）或已改变，位置型 NPY 和使用数字行号的
CSV 不自动挂载，并在标注诊断中说明原因。带完整样本名称的 CSV 继续按名称严格对齐，ASDF
继续执行自身来源兼容性校验；文件重命名不被猜测为同一 CSV 样本身份。
被拒绝的标注关联不再写入自动恢复清单，避免下次启动误用新来源身份重新挂载。
启动恢复以保存的关联清单为准，NPY companion 自动发现不能绕过上述身份校验或重新引入已拒绝的关联。
用户仍可显式重新挂载标注。恢复失败或尚未完成的来源保留原标注路径及原身份，不更新为当前身份。

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

源加载失败详情直接显示在 Files 中，包括失败路径、错误语义和底层诊断，可复制或显式忽略。
状态栏的 `Load failed` 点击后打开并聚焦 Files，不直接忽略错误；详情沿用加载事务的失败生命周期，
不会因查看或复制而清除。失败详情是操作反馈，不是新增来源记录。
加载失败时保留会话与继续展示旧样本是两个独立决策。#138 的行为契约如下：

| 场景 | 契约 |
| --- | --- |
| 当前显式打开失败 | 保留已加载来源及 workflow，清空当前光谱展示；Information 显示失败目标和诊断，不显示旧样本元数据 |
| 当前集合的目标成员失败 | 保留集合、真实数量和标注；不展示上一条，也不允许当前样本标注操作作用于上一条 |
| 非当前请求的后台失败 | 记录诊断，不清空当前成功展示 |
| 过期或取消的 completion | 不改变当前展示，也不覆盖当前错误 |
| 首次打开新来源失败 | 不加入恢复清单 |
| 忽略错误 | 仅确认诊断，不隐式重新激活旧样本 |

失败后的空展示一直保持到用户显式选择有效来源／样本，或当前重试成功。
查看、复制、忽略诊断以及后台维护均不是重新显示样本的触发条件。
会话级任务、数量和保存状态仍可查看；当前样本的元数据、标注值和写入入口不可回退到旧样本。
Information 中的失败目标属于本次展示状态，忽略通知不会把它替换为旧样本信息。

回归覆盖：`tests/source_collection_activation_transaction_tests.cpp` 验证激活来源、加载顺序、
连续启动和移除后的持久化；`tests/widget/source_files_widget_tests.cpp` 验证无光谱时的失败行、
禁用选择、悬停提示和可用的移除按钮。

## Files 面板接收 Explorer 文件拖放

Explorer 中的文件和文件夹可拖到当前实例的 Files 内容区域；兼容的文件系统拖放经过时，
面板显示高亮边框。标题栏、其他面板、被其他窗口遮挡的区域、折叠或隐藏的 Files 面板
不是接收目标；弹窗阻止操作时也不接收。Files 停靠和独立浮动时使用同一行为。

使用 Windows OLE `CF_HDROP` 接收 UTF-16 路径，包括空格和 Unicode；不接收虚拟文件或
应用内部 ImGui payload。每批最多 256 条路径，超限整批拒绝，不截断或部分提交。
按 shell 提供的顺序调用现有应用内 `OpenSource`，由原加载队列限制并发、处理取消、
身份、重复、激活、会话和失败诊断。不做新目录枚举、格式识别或多路径来源合并。
不支持或不可访问的来源由原流程报告具体路径，已成功加入的来源和现有会话仍可使用。

这不是外部启动请求：不经过实例路由、不启动其他进程、不使用 automation named pipe，
也不应用外部启动的“打开父文件夹”设置。普通已有成员解析仍按下节规则执行。

Annotations 面板支持拖入标注**文件**，等价于其“添加文件”操作；不支持文件夹。
悬停时根据 Shell 对象属性拒绝文件夹和混合了文件夹的选择，整批不导入。
没有可添加标注的活动来源、面板隐藏/折叠或被遮挡时不接收。
多份标注文件按 shell 顺序逐个通过原导入流程处理，沿用格式、样本对应关系、重复和错误诊断。
有效标注保留，失败文件的诊断显示在 Annotations 中；不新建来源、不切换当前光谱。
Files 与 Annotations 共用原生窗口或分别浮动时，以松手位置确定目标面板。

## 显式打开已有光谱成员

应用内打开、Files 面板提交的打开和转发到当前实例的单文件打开，共用 source activation
事务的成员解析。只按已有文件成员清单和路径身份匹配；不按显示名称、路径前缀或 NPY 行名推断。
多个来源包含同一成员时，优先当前来源，否则按 Files 清单顺序选择。健康来源复用已有
listing、上下文和可用的样本快照；需要解码时只加载目标样本，不隐式重载整个来源。
过期或不唯一的成员映射回退到普通文件打开，不新增跨实例来源注册表。

显式打开保留 annotation、sample filter 和排序。目标不在当前 sample filter 结果中时，
仍显示真实 source row，序列位置为空，Previous/Next 不可用；不添加临时例外。
普通行号、名称及序列位置定位仍受当前序列约束，定位有效样本后恢复序列游标。
关闭 sample filter 保留显式打开的样本，并按现有排序取得完整序列中的位置。

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
已有集合中的成员按上述显式打开规则处理。没有可复用集合时，外部启动的父目录解释规则保持独立；
若成员已从目录消失、无法加载或被保存的 sample filter 排除，沿既有错误路径反馈。
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
