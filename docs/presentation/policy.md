# 显示呈现产品合同与验证矩阵

状态：产品要求已确认；Composition 主路径已接入生产 renderer，本机自动化与正式交互视觉验收通过；严格尾部延迟门禁及未具备硬件的场景仍未通过。

本文定义 Spectiary 在不同显示器、刷新率和 Windows 自动刷新率设置下的产品行为。
它区分产品不变量、实现候选和已经取得的证据；没有真实硬件证据的场景不得标记为通过。

## 产品合同

Spectiary 在存在持续画面变化时，尝试使用窗口所在显示目标当前允许的最高交互刷新节奏，
同时保持无可见撕裂。空闲且没有画面变化时保持事件驱动，不为维持刷新率而制造空帧。

“当前允许的最高刷新节奏”不等于擅自切换 Windows 显示模式。若用户将支持高刷的显示器
固定为 60 Hz，普通窗口模式下的目标是无撕裂 60 Hz；应用可以记录系统模式对刷新率的
限制，但不能静默修改用户的显示设置或进入独占全屏。

| 显示环境 | 目标行为 |
|---|---|
| 固定 60 Hz | 有更新时无撕裂 60 Hz |
| 固定 120 Hz | 有更新时无撕裂 120 Hz |
| 固定 144 Hz 或其他高刷 | 有更新时无撕裂地跟随当前活动刷新率 |
| DRR 60/120 | 延迟敏感交互时请求 120 Hz；空闲时允许系统回到 60 Hz |
| DRR 关闭、固定高刷 | 不依赖 DRR boost，跟随当前固定刷新率 |
| 主窗口或 detached viewport 换屏 | 按各自当前显示目标重新判断能力和目标节奏 |
| 无画面更新 | 不连续渲染，不用空帧维持高刷 |

## 独立面板缩放资源策略

独立 ImGui viewport 默认在 Composition 后端使用增量 buffer 替换。初始创建三个精确尺寸
槽位；缩放仅更新最新请求尺寸，每个 viewport 每个 ImGui 帧最多替换一个可用且非最后提交
的槽位。复用尺寸匹配的可用槽位，无可用槽位时跳过本帧，不增加阻塞等待或渲染线程。

每个 viewport 的逻辑 RGBA8 像素预算为 256 MiB，包含三个已安装槽位和替换期间的临时
槽位；它不是进程、驱动或 compositor 的实际显存上限。尺寸不支持、预算不足或分配/提交
失败时走已有 DXGI 回退，该窗口生命周期内不反复尝试 Composition。大尺寸窗口可能因此
较早回退，DXGI 的既有内存与呈现策略不受此逻辑预算约束。

主窗口策略、窗口 ownership、普通 HWND 样式和反馈读取频率保持既有行为。系统 API、
驱动、资源释放和 Present 仍可能产生等待；本机正向证据不承诺固定收益或跨硬件无卡顿。
验证和维护边界见[生产启用记录](live-resize/evidence/20260917-production-enablement.md)。

## 降级顺序

1. 首选：当前环境允许的最高交互刷新率，并且无撕裂。
2. 若最高刷新率与无撕裂不能同时实现：保持最高刷新率，允许可见撕裂。
3. 只有最高刷新率路径不可用或不稳定时，才降低刷新率；在降低后的同一刷新率上仍优先无撕裂。

这是一条字典序优先级：先比较实际刷新率，再比较同一刷新率下是否无撕裂。对应的
降级等级为：

| 等级 | 状态 | 相对优先级 |
|---|---|---:|
| `none` | 最高刷新率、无撕裂 | 最高 |
| `tearing_at_target_rate` | 最高刷新率、允许撕裂 | 次高 |
| `reduced_rate_tear_free` | 降低刷新率、无撕裂 | 再次 |
| `reduced_rate_tearing` | 降低刷新率、仍有撕裂 | 最低 |

任何非 `none` 状态都必须被明确选择和记录。尤其是 `ALLOW_TEARING` 不能因为 capability
存在就静默启用；策略必须记录它是为了维持目标刷新率而采用的第一层降级。

这里的“降级”既包括应用主动选择较低节奏，也包括系统、驱动、显示器或电源策略没有满足
应用请求。两类情况都必须可观测，但应在日志中区分 `policy_fallback` 和
`external_constraint`，避免把用户选择的固定 60 Hz 错报成应用故障。

## 呈现策略 seam

最终实现应由一个拥有明确策略的 renderer module 统一管理主窗口和 detached viewport 的
swap chain。UI 和 plot 只表达“当前是否存在延迟敏感交互”，不能分别组合 sync interval、
Present flags、DRR boost 或自定义 duration。

该 module 至少统一负责：

- 当前 swap chain 对应的显示目标及其变化；
- compositor clock 初始化、boost 请求和成对释放；
- DRR vblank virtualization 能力和调用结果；
- 同步 Present、custom present duration 等 adapter 的能力查询和选择；
- 主窗口与 detached viewport 一致的刷新率/撕裂降级优先级；
- 策略选择、外部约束、实际节奏和降级原因的 telemetry。

`DXGI_FEATURE_PRESENT_ALLOW_TEARING` 是系统级能力信号，不足以证明当前显示器已启用 VRR，
也不足以证明当前窗口使用 `ALLOW_TEARING` 后没有可见撕裂。它只能证明该 Present flag
可以合法使用；选择可撕裂路径还必须有“目标刷新率下的无撕裂路径不可用”这一策略原因，
并记录为 `tearing_at_target_rate` 或 `reduced_rate_tearing`。

## 可观测性合同

呈现策略不得只在失败时写一条无法复现的错误。应在策略发生变化时写结构化事件，而不是
逐帧刷日志。触发点至少包括：

- 启动和 swap chain 创建；
- 主窗口或 detached viewport 的目标显示器变化；
- `WM_DISPLAYCHANGE` 或相关显示配置变化；
- compositor clock boost 请求和释放；
- custom duration 请求、系统批准、撤销或拒绝；
- 策略 adapter 变化；
- 观测节奏持续低于目标。

建议的诊断字段如下；具体 JSONL 形状由实现测试确定：

| 类别 | 字段 |
|---|---|
| 目标身份 | swap chain role、窗口句柄标识、monitor device、adapter/output 标识 |
| 显示环境 | active mode Hz、DWM/compositor Hz、当前分辨率、是否主显示器 |
| 目标 | desired interactive Hz、selected target Hz、是否请求最大可用节奏 |
| DRR | compositor clock available、boost requested/active、HRESULT、vblank virtualization 状态 |
| duration | support query、requested/closest/approved duration、HRESULT |
| Present | adapter 名称、sync interval、flags 的符号名和数值、tearing permitted、tear-free expected |
| 结果 | observed cadence、degradation level、policy degraded、constraint kind、reason、首次发生时间 |

降级或约束原因至少要能区分：

- 当前 Windows 显示模式限制刷新率；
- compositor clock 或 DRR boost 不可用、失败或未生效；
- vblank virtualization 控制不可用或失败；
- custom duration 不支持、请求失败或没有获批；
- 目标刷新率下的无撕裂路径不可用，因而启用 tearing；
- 目标刷新率路径不可用或不稳定，因而降低刷新率；
- 目标显示器无法识别或窗口正在换屏；
- 实测刷新节奏持续低于所选目标；
- 不同刷新率 swap chain 之间出现阻塞耦合。

应用不能仅凭 API 返回 `S_OK` 声明达到目标。`requested`、`approved` 和 `observed` 必须分开。
若声明 `tear-free expected=true`，还需要真实视觉门禁；若允许 tearing，则必须在策略事件中
显式记录。Present 调用完成时间不能单独证明 scan-out 无撕裂。

## 当前证据边界

当前硬件上的 Composition 主路径已有自动化和正式交互视觉证据；严格尾部延迟门禁尚未全部通过。外接显示器、混合刷新率、多适配器和通用 VRR 等缺少实测的场景仍保留在下方验证矩阵，不以本机结果推定通过。

早期 DXGI、custom-duration、Composition 探针和触控板调度的逐次日志、数据与判断保存在[呈现策略历史证据](evidence/presentation-history.md)。独立面板缩放的现行结论、失败候选与生产启用证据集中在[窗口缩放调查](live-resize/README.md)；主窗口原生缩放刷新仍是独立待办。

## 已选实现与保留回退

生产 renderer 现在为每个 HWND 独立拥有 `D3D11WindowPresentation`：主窗口和 detached
viewport 共享同一策略边界，UI/plot 不再组合具体 Present API。默认路径是标准 Windows
Composition Swapchain：从窗口所在 active display path 的物理刷新率计算 preferred duration，
使用显式 source rect、三张 displayable SDR/P709 buffer，并通过 tagged
IndependentFlip statistics 观测实际显示时长。

策略选择和生命周期如下：

1. 首选 Composition Swapchain；固定刷新率与 DRR 使用同一路径，不按内外屏写特例。
2. 初始化、duration 更新、resize、buffer acquire 或 Present 的不可恢复错误均在该 HWND
   上切到既有显式 SDR DXGI swap chain，并记录原 backend、失败操作和 HRESULT。
3. DXGI 回退在 compositor-clock 交互节奏可用且系统支持 tearing 时使用
   `Present(0, ALLOW_TEARING)`，记录 `tearing_at_target_rate`；否则使用同步 Present，记录
   `reduced_rate_tear_free`。固定 60 Hz 等用户/系统模式限制单独记录为
   `system_refresh_constraint`，不伪装成应用降级。
4. `WM_DISPLAYCHANGE` 使用 500ms 合并更新，窗口退出 move/size 或 viewport 换屏也会重新查询
   各自目标；不修改 Windows 全局显示模式。
5. 主窗口的 buffer acquire 最多等待 1 秒；缓冲仍不可写时只跳过整个当前应用帧并请求后续帧，
   不执行 draw、detached viewport render、Present 或 presentation completion。已累计的
   `buffer_acquire_skipped` 保留在聚合 feedback 中；compositor-clock 模式继续等待下一次 tick
   许可，非 clock-paced 模式的连续重试仍由每次 buffer acquire 的有界等待节流。
6. detached viewport 使用非阻塞 buffer acquire；缓冲暂不可写时只跳过该 viewport 当前帧并
   累计 `buffer_acquire_skipped`，避免慢显示目标在 UI 线程上阻塞其他窗口。

呈现状态只在策略转换时记录；实际 feedback 在首次 IndependentFlip、异常、每累计 120 次提交
或 120 次 buffer acquire 跳过时聚合记录，避免逐帧日志。原有 custom-duration 和
vblank-virtualization 实验保留为历史证据，不再进入生产策略分支。

## 验证矩阵

| 场景 | 当前硬件可测 | 验收重点 | 状态 |
|---|---:|---|---|
| 内屏 DRR 自动，鼠标 pan | 是 | boost 高档、最大节奏、无撕裂、释放 | 生产 Composition 正确识别 DRR 虚拟约 60/物理约 120Hz；普通、沉浸及回 dock 后主 Plot 的有效交互 feedback 均为 83333 duration，视觉无撕裂且无功能异常；Present interval p50/p95 8.322/8.855ms，严格 p95 门禁仍 MISS；tick permission 与 invalidation 解耦后，静止按住不再空提交且恢复移动正常 |
| 内屏 DRR 自动，触控板 pan/zoom | 是 | 原生输入延迟、惯性、无撕裂、释放 | pan/zoom 确认获批后约 120Hz 且无撕裂；attached HWND 的 Direct Manipulation standalone pump 按 compositor tick 合并，`Poll()` 保留自身 `Update()`。`165754`/`171212` 否定 single-update gate；恢复无 gate 后，`172023` 以 1869 gesture/1869 frame 复验连续 pinch、静止、同接触恢复及惯性均无异常，input→Present p50/p95 8.183/9.319ms；短手势重获批 p50 约 75–82ms；锚点几何待独立验收 |
| 内屏固定 60 Hz | 是 | 无撕裂、不误报 DRR 失败 | `Present(1, 0)` 与 composition-swapchain v13 均真实约 60Hz、无撕裂；后者正确识别非 DRR 的虚拟/物理约 60Hz，主观较流畅，归类 `system_refresh_constraint` |
| 内屏固定高刷（系统提供的档位） | 是 | 跟随活动刷新率、无撕裂 | 固定 120Hz：`Present(1, 0)` 与 composition-swapchain v13 均真实约 120Hz、流畅无撕裂；后者正确识别非 DRR 的虚拟/物理约 120Hz |
| 运行中切换自动/固定刷新率 | 是 | 策略重新感知、无 stale duration | 既有 HWND 路径双向均捕获 `WM_DISPLAYCHANGE`；composition-swapchain v14 连续跨固定 120/DRR 120/固定 60/DRR 120/固定 120，四次 duration 更新均 `S_OK`、实际扫描正确跟随且主观无异常；客观记录 119–340ms 提交与 158–598ms 显示 handoff，归类 `system_mode_switch_handoff` |
| 普通、沉浸、同屏 detached viewport | 是 | 所有 swap chain 行为一致 | 自动测试 18/18；正式联合交互中主窗口与 detached 均使用 Composition，全部 drag feedback 为 83333 duration，视觉无撕裂，移动/resize/拖回成功；早期联合交互记录的 detached resize 卡顿已由后续增量 buffer 调查推进：普通样式 A 复验、有限生命周期/窗口行为/资源检查为正，v0.10.0 已默认启用独立面板策略；收益、跳帧/回退及资源限制见[窗口缩放结论](live-resize/README.md)。主窗口原生缩放刷新仍单独留在 #101 |
| 外接固定 60/75 Hz | 否 | 同步跟随、正确记录约束 | 延后硬件验证 |
| 外接固定 120/144/165 Hz | 否 | 最大活动刷新率、无撕裂 | 延后硬件验证 |
| 内外屏不同刷新率并同时显示 | 否 | 慢屏不拖住快屏、各目标无撕裂 | 延后硬件验证 |
| 主窗口跨屏移动 | 否 | 目标切换和策略重选 | 延后硬件验证 |
| detached viewport 分布在不同屏幕 | 否 | per-target 策略、无交叉阻塞 | 延后硬件验证 |
| FreeSync/G-Sync/通用 VRR 显示器 | 否 | VRR 是否真实启用、范围外行为、无撕裂 | 延后硬件验证 |
| 多 GPU、扩展坞或 eGPU | 否 | adapter/output 迁移和 capability 重建 | 延后硬件验证 |
| 不支持相关 Windows 11 API 的环境 | 当前不可代表 | 先保最高节奏、记录 tearing；最后才降率 | 延后环境验证 |

延后项是正式验收债务，不是已支持声明。未来取得对应硬件后，应沿用同一真实数据、交互动作、
日志字段和视觉门禁补齐结果。
