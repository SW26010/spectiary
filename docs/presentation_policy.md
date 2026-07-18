# 显示呈现产品合同与验证矩阵

状态：产品要求已确认；最终 Windows 呈现实现仍在实验验证中。

本文定义 SpecForge 在不同显示器、刷新率和 Windows 自动刷新率设置下的产品行为。
它区分产品不变量、实现候选和已经取得的证据；没有真实硬件证据的场景不得标记为通过。

## 产品合同

SpecForge 在存在持续画面变化时，尝试使用窗口所在显示目标当前允许的最高交互刷新节奏，
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

- commit `8282a95` 的 compositor-clock tick 可以在本机 DRR 交互时形成约 120 Hz 的提交节奏，
  交互路径使用 `Present(0, ALLOW_TEARING)`，真实 pan 可见稳定水平断层。它满足约 120 Hz
  节奏，但只能归类为 `tearing_at_target_rate`，不是首选的最高刷新率无撕裂状态。
- 临时 `Present(1, 0)` A/B 消除了撕裂，但约 60 Hz，只能归类为
  `reduced_rate_tear_free`；按产品优先级低于保持约 120 Hz 的 tearing 路径。
- `logs/specforge-profile-20260718-054947.jsonl` 完成了候选 1 的有效鼠标 pan 验证：
  `DXGIDisableVBlankVirtualization()` 在创建 swap chain 前返回 `S_OK`，沉浸模式下 DWM
  从 60.000 Hz 升至 120.000 Hz，`Present` 使用 sync interval 1、flags 0，视觉无撕裂；但
  30.176s 拖动窗口内 `Present interval` p50/p95 仍为 16.660/17.704ms，ImPlot pan sample
  p50/p95 为 16.686/17.807ms，input→Present p95 为 16.448ms。由此可排除“仅禁用 vblank
  virtualization 即可使同步 Present 跟随 DRR 120 Hz”这一假设；API 成功与 DWM 升档均未
  改变该 swap chain 的约 60 Hz 实际节奏。本机结果仍归类为 `reduced_rate_tear_free`。
- `logs/specforge-profile-20260718-060018.jsonl` 完成了候选 2 的有效鼠标 pan 验证：探针
  确认交互路径为 `Present(0, 0)`，24.210s 拖动窗口内 boost 请求返回 `S_OK` 且
  `active=true`，但 compositor tick 仅增加 1453 次（约 60.0Hz），DWM timing 始终约
  60.015Hz；`Present interval` p50/p95 为 16.664/17.536ms，ImPlot pan sample p50/p95
  为 16.672/17.460ms，input→Present p95 为 16.412ms，视觉无撕裂。由此可排除
  “compositor-clock paced `Present(0, 0)` 可在本机 DRR 下维持 120Hz”这一假设；移除
  `ALLOW_TEARING` 后 compositor clock 本身也只产生约 60Hz tick。本机结果同样归类为
  `reduced_rate_tear_free`。
- `logs/specforge-profile-20260717-195210.jsonl` 的 custom-duration 探针成功请求并最终获批
  `83333` 个 100 ns 单位，且本机视觉观察无撕裂；该次分析没有捕获有效的 ImPlot 左键
  pan-drag 窗口，因此只能作为本机 capability 与视觉证据，不能作为完整性能验收。
- `logs/specforge-profile-20260718-060956.jsonl` 完成了候选 3 的首个有效鼠标 pan 验证：
  `SetPresentDuration(83333)` 返回 `S_OK`，约 187.5ms 后 `ApprovedPresentDuration` 变为
  `83333` 并稳定保持至释放，22.816s boost 窗口内 compositor tick 为 2729 次（约
  119.61Hz）；`Present interval` p50/p95 为 8.331/8.575ms，ImPlot pan sample p50/p95
  为 8.332/9.079ms，input→Present p95 为 8.310ms。视觉无撕裂且主观为高刷新率，证明
  本机内屏 custom-duration adapter 首次同时满足“约 120Hz + 无可见撕裂”。严格
  8.3333ms p95 总门禁仍为 FAIL：鼠标输入间隔 p95 8.793ms、pan sample p95 9.079ms、
  Present interval p95 8.575ms；因此当前是成功候选，不是完整验收通过。
- 同一日志中释放后 `SetPresentDuration(0)` 返回 `S_OK`、boost 为 `active=false`；随后
  37.015s 只有 236 次 Present，30 个间隔超过 50ms、10 个超过 500ms，最长空档
  4.992s。这与“鼠标不动时帧号间断缓慢更新”的观察一致，属于预期的事件驱动空闲，
  不是 custom duration 未释放或持续降频；静态画面不需要连续 Present。
- `logs/specforge-profile-20260718-061733.jsonl` 重复确认候选 3：48.947s boost 窗口内
  5865 次 compositor tick（约 119.82Hz），duration 在请求后约 191.6ms 获批并稳定保持；
  `Present interval` p50/p95 为 8.332/8.624ms，input→Present p95 为 8.348ms，视觉仍无
  撕裂且主观为高刷新率。按住左键期间存在 4165.7ms 无任何 pointer move 的静止段，
  该段仍完成 500 次 Present（约 120.03Hz，最大间隔 11.1ms），证明“按住但不移动”
  不会退出高刷新路径。松开后 15.172s 只有 56 次 Present、最长空档 4.988s，再次证明
  间断帧号来自预期空闲。严格 8.3333ms p95 门禁仍有轻微 MISS，因此候选已具备可重复
  功能证据，但尚未完成统计门禁与其他显示模式验证。
- `logs/specforge-profile-20260718-063654.jsonl` 完成了候选 3 的纯触控板平移验证；日志中
  有 4158 个 `touchpad.gesture` pan、没有鼠标左键或 ImPlot mouse pan 样本。36.352s
  boost 窗口内 compositor tick 为 4359 次（约 119.91Hz），custom duration 在 75.4ms
  后获批为 `83333` 并保持至释放；`Present interval` p50/p95 为 8.32/9.11ms，触控板
  input→Present p50/p95 为 8.23/9.01ms，axis update interval p50/p95 为 8.33/9.28ms。
  视觉无撕裂，但主观刷新率不高。日志将差异定位在触控板运动而不是呈现回落：稳定同向
  移动的触控板输入间隔 p50/p95 为 8.33/9.15ms；69 次方向反转的间隔 p50 为 25.25ms，
  全程 61 个超过 20ms 的输入空档中有 44 个跨越方向反转、51 个邻接近零位移。Present
  在这些空档中仍维持约 120Hz，gesture consume→Present p95 仅 0.73ms。因此本轮证明
  DRR custom-duration 路径也覆盖触控板 pan；主观低刷新感受还需用受控单向手势区分
  Direct Manipulation 换向停顿和更广泛的触控板交互问题。
- `logs/specforge-profile-20260718-064412.jsonl` 以 18 段分离的单向触控板滑动复测上述
  主观差异。18 个 touchpad boost 窗口全部成功获批 `83333` duration，总窗口时间
  28.737s、3398 个 compositor tick（包含每段启动/释放边界时约 118.25Hz）；duration
  获批后的 `Present interval` p50/p95 为 8.33/9.04ms。稳定同向、非惯性 transform delta
  间隔 p50/p95 为 8.34/9.30ms，窗口内方向反转从连续来回测试的 69 次降至 8 次；61 个
  超过 20ms 的 transform 空档中有 57 个邻接近零位移，符合分段滑动的起停边界。视觉
  无撕裂，主观刷新率由“不高”改善为“还行”，支持此前感受主要来自来回换向和停速，
  而不是 renderer 回落到 60Hz。另记录 adapter acquisition latency：每段重新请求后
  duration 获批延迟 p50/p95 为 75.0/103.9ms，获批前 Present interval p50 为 16.0ms，
  获批后为 8.33ms；该过渡是独立的生命周期指标，最终策略应结构化记录。
- `logs/specforge-profile-20260718-065113.jsonl` 完成了候选 3 的纯触控板 pinch zoom 验证：
  2833 个 `touchpad.gesture` 全部为 zoom，轴限制变化同为 2833 次，没有 pan 混入。
  28 个 touchpad boost 窗口全部成功获批 `83333` duration，总窗口时间 28.804s、3357 个
  compositor tick；由于手势窗口中位长度仅 434ms 且反复启动/释放，包含边界的聚合 tick
  频率约 116.55Hz。duration 获批后的 `Present interval` p50/p95 为 8.32/9.08ms，
  input→Present p50/p95 为 8.23/9.28ms，稳定同向 zoom delta 间隔 p50/p95 为
  8.34/9.63ms；视觉无撕裂，主观刷新率“还行”。本轮再次量化了短手势 acquisition
  latency：duration 获批延迟 p50/p95 为 82.0/132.7ms，获批前 Present interval p50
  15.83ms，获批后 8.32ms，等待获批时间合计占 boost 窗口的约 8.46%。日志证明缩放
  transform 与轴更新链路完整；锚点几何没有自动 telemetry，本轮只能记录未收到可见
  跳变或漂移报告，不能据此声明自动锚点验收通过。
- `logs/specforge-profile-20260718-062438.jsonl` 完成了内屏固定 120Hz 的纯垂直同步基准：
  探针确认没有使用 custom present duration 或 DRR 专用呈现变量，主路径为
  `Present(1, 0)`；Windows mode 为 120.000Hz，DWM 为 120.032Hz，27.930s boost 窗口内
  compositor tick 为 3353 次（约 120.05Hz）。`Present interval` p50/p95 为
  8.325/8.992ms，ImPlot pan sample p50/p95 为 8.332/8.943ms，input→Present p95 为
  8.361ms；视觉无撕裂且主观为高刷新率。这确认固定高刷模式下普通同步 Present 已能
  跟随活动刷新率，不需要内屏专用 custom-duration adapter。功能目标已满足，但严格
  8.3333ms p95 门禁仍为 FAIL；节奏能力判定与尾部抖动验收必须分别报告。
- `logs/specforge-profile-20260718-062957.jsonl` 完成了同一纯垂直同步路径的内屏固定
  60Hz 对照：探针仍为 `Present(1, 0)`，Windows mode 为 60.000Hz，DWM 为 60.015Hz，
  27.249s boost 窗口内 compositor tick 为 1635 次（约 60.002Hz）。`Present interval`
  p50/p95 为 16.673/17.548ms，ImPlot pan sample p50/p95 为 16.670/17.454ms，
  input→Present p95 为 16.405ms；视觉无撕裂，主观刷新率不高。该结果与固定 120Hz
  基准共同确认普通同步 Present 会正确跟随当前活动模式。固定 60Hz 是用户选择的系统
  约束，不是应用主动降级；功能目标已满足，严格 16.6667ms p95 抖动门禁仍为 FAIL。
- `logs/specforge-profile-20260718-065840.jsonl` 在同一个纯 `Present(1, 0)` 进程中完成
  DRR 自动到固定 120Hz 的运行时切换。切换前沉浸模式主拖动窗口为 12.594s，compositor
  tick 约 60.01Hz，`Present interval` p50/p95 为 16.68/17.51ms，input→Present p95
  为 16.40ms，视觉无撕裂但流畅度一般。退出交互后 boost 已释放，随后
  `WM_DISPLAYCHANGE` 明确记录 Windows mode 从 60Hz 变为 120Hz、DWM 变为 120Hz；该
  消息没有伴随 render-target resize。切换后同一 swap chain 的 12.242s 主拖动窗口为
  120.08Hz，`Present interval` p50/p95 为 8.34/8.92ms，input→Present p95 为 8.29ms，
  视觉仍无撕裂且明显更流畅。由此确认普通同步 Present 无需重建 swap chain 即可跟随
  固定模式变化，现有 `WM_DISPLAYCHANGE` telemetry 也能捕获该外部约束变化；最终策略仍
  需在该事件后重新评估 adapter，而不能只更新日志。
- `logs/specforge-profile-20260718-070534.jsonl` 在同一个 custom-duration 探针进程中完成
  固定 120Hz 到 DRR 自动的反向运行时切换，并验证 duration 释放与重新获批。固定 120Hz
  阶段的沉浸模式拖动为 13.550s，duration 请求后 135.5ms 获批 `83333`，`Present
  interval` p50/p95 为 8.332/8.681ms，input→Present p95 为 8.304ms；交互结束时
  `SetPresentDuration(0)` 返回 `S_OK`。保持进程运行并切换到 DRR 自动后，
  `WM_DISPLAYCHANGE` 记录 Windows mode 从 120Hz 变为 60Hz；随后两段拖动共 10.558s，
  各自重新请求 duration 并在 191.5ms、275.0ms 后获批。两段 `Present interval` p50/p95
  分别为 8.334/8.809ms、8.336/8.635ms，input→Present p95 分别为 9.081ms、8.375ms，
  pan sample 中位节奏均约 120Hz。每次新请求后的首批统计都明确为
  `approved_duration=0`，随后才变为 `83333`；每次释放的 duration reset 也都返回
  `S_OK`，因此不是沿用上一次获批状态。用户报告切换前后均无撕裂且流畅。整份日志按
  8.3333ms 的严格 p95 总门禁仍为 FAIL（Present interval p95 8.683ms、input→Present
  p95 8.345ms），应继续把功能能力与尾部抖动验收分开报告。本轮确认现有生命周期代码
  能在同一 swap chain 的固定高刷→DRR 变化后释放并重新取得 custom duration；最终策略
  仍应依据新显示环境重新选择 adapter，而不是在固定高刷下无条件请求 custom duration。
- `logs/specforge-profile-20260718-071148.jsonl` 证明同一 DRR 内屏、同一 custom-duration
  探针在普通 Plot Panel 中没有取得 120Hz。两段鼠标拖动分别为 7.106s、23.157s；第一段
  窗口 render target 为 2240×1435，第二段在最大化到工作区 2880×1760 后进行。两段
  compositor boost 均完整有效，tick 分别为 120.02Hz、120.04Hz，
  `SetPresentDuration(83333)` 和释放 `SetPresentDuration(0)` 都返回 `S_OK`；但 445 次与
  1406 次逐帧统计的 `ApprovedPresentDuration` 全部为 0。对应 `Present interval`
  p50/p95 分别为 16.654/16.994ms、16.657/16.903ms，input→Present p95 分别为
  16.653ms、16.597ms。用户观察无撕裂但流畅度一般。由此确认直接原因不是 Plot 工作量、
  输入频率或 compositor boost 失败，而是系统没有批准普通窗口的 custom duration；
  `SetPresentDuration` 返回 `S_OK` 不能当作已达到目标。最大化到不含任务栏的工作区仍不
  足以获批。沉浸模式同时改变了 borderless fullscreen 覆盖范围和 Plot UI，下一轮用
  保持普通 Plot Panel 的 F10 borderless-fullscreen A/B 单独验证窗口覆盖条件。
- `logs/specforge-profile-20260718-072810.jsonl` 是上述 F10 A/B 的首次尝试，但没有形成
  有效的 borderless-fullscreen 对照。duration probe 标签确认运行的是正确诊断二进制；
  日志却没有任何 `toggle_borderless_fullscreen` 探针，反而记录了一次成对的
  `immersive_plot=true/false` 与 fullscreen 进入/退出，持续仅 629.1ms，且该全屏区间内
  没有 pan sample。因此应用实际收到的是沉浸切换，而不是诊断 F10 路径；当前 input
  telemetry 不记录键盘 virtual key，尚不能从日志解释 F10 为何没有触发。用户指出同一
  键盘的 F11 可以正常进入沉浸模式，因此不把该现象归因于简单的 Fn 功能层问题。
  四段有效拖动均发生在 2880×1760 普通窗口，合计 24.854s；1489 次 duration probe 的
  `ApprovedPresentDuration` 全部为 0，`Present interval` p50/p95 为
  16.661/17.167ms，用户仍观察无撕裂但流畅度一般。本轮再次复现普通窗口约 60Hz，
  但不能用于判断“仅 borderless fullscreen 覆盖是否足以获批”；该单变量实验仍待完成。
- `logs/specforge-profile-20260718-074250.jsonl` 用 v2 的 `Ctrl+Shift+Enter` 完成了保持普通
  UI、只切换主窗口 borderless fullscreen 状态的有效 A/B。组合键探针记录
  `virtual_key=13`，render target 从最大化工作区 2880×1760 变为覆盖内屏的
  2880×1800，没有进入 immersive Plot。切换前 21.578s 普通窗口拖动中 compositor tick
  为 120.03Hz，但 1300 次 duration probe 全部未获批；`Present interval` p50/p95 为
  16.662/16.914ms，input→Present p95 为 16.623ms。borderless fullscreen 中的主拖动
  持续 14.599s，duration 在请求后 199.8ms 从 0 重新获批为 `83333`；`Present interval`
  p50/p95 为 8.332/8.540ms，input→Present p95 为 8.287ms。用户报告两阶段均无撕裂，
  普通最大化流畅度一般，borderless 后流畅。由此排除“必须使用沉浸 Plot UI”这一假设，
  并证明本机 Windows 的 custom-duration 批准取决于 borderless-fullscreen 窗口状态。
  该状态同时改变了窗口 style 和任务栏覆盖范围，因此当前证据只能称它为充分条件，不能
  进一步断言唯一条件就是客户区达到 2880×1800。退出 borderless 后的一段 1.405s 短拖动
  虽仍暂时约 120Hz，但 167 次批准值已全部回到 0，属于显示切换滞留，不能作为普通窗口
  长期高刷证据。最终策略必须持续验证批准状态；普通窗口持续未获批时，应按产品顺序转入
  `tearing_at_target_rate`，而不是静默停留在约 60Hz `reduced_rate_tear_free`。
- `logs/specforge-profile-20260718-075134.jsonl` 原计划验证普通窗口的
  `tearing_at_target_rate`，但所选 `build/final-drr-verify-debug/SpecForge.exe` 实际是历史
  同步 Present 构建，不能作为 tearing adapter 证据。15.122s 拖动期间 boost 正常
  `active=true`，compositor tick 增加 1815 次（约 120.02Hz），但只有 907 个 pan sample，
  `Present interval` p50/p95 为 16.666/16.898ms，Present 调用自身 p50/p95 为
  13.882/14.761ms；这组“120Hz tick、约 60Hz frame、Present 明显阻塞”的组合证明
  实际仍是同步等待。用户观察无撕裂且流畅度一般，与该结果一致。本轮再次确认普通窗口的
  `reduced_rate_tear_free` 基准，但不能判定 tearing 降级成功或失败。后续诊断二进制必须
  在日志中自描述 sync interval、flags、tearing capability 和预期降级等级，不能依靠历史
  目录名或构建时间推断呈现策略。
- `logs/specforge-profile-20260718-075644.jsonl` 使用自描述诊断构建完成了普通窗口的有效
  `tearing_at_target_rate` 验证。启动探针明确记录
  `action=force_present0_allow_tearing`、`present_sync_interval=0`、`present_flags=512`
  (`DXGI_PRESENT_ALLOW_TEARING`)、`tearing_supported=true` 和
  `tear_free_expected=false`。18.522s 的 boost 区间累计 2223 个 compositor tick，实测
  120.00Hz；18.536s pan 中 `Present interval` p50/p95 为 8.336/9.155ms，pan sample
  interval 为 8.337/9.023ms，input→Present p95 为 8.424ms，Present 调用自身 p50/p95
  仅 0.041/3.154ms。用户观察“流畅有撕裂”。这与上一轮同步路径“约 60Hz、无撕裂、
  Present 明显阻塞”形成同窗口 A/B，确认普通窗口在本机可通过第一层产品降级维持约
  120Hz，但代价是可见撕裂；不能归类为首选成功状态。本轮严格 8.3333ms p95 门禁仍轻微
  超限，需与 adapter 可用性结论分开记录。分析器显示的 DWM 60Hz 来自后续 `resize`
  环境快照，并不代表交互区间节奏；报告必须优先给出交互窗口内的 observed cadence。
- `logs/specforge-profile-20260718-080234.jsonl` 将 Spectrum 拖出为同屏 detached viewport，
  使用与上一轮相同的 `Present(0, DXGI_PRESENT_ALLOW_TEARING)` 降级。两段 pan 共
  16.764s；主段 15.320s 内 compositor tick 为 1839（120.04Hz），`Present interval`
  p50/p95 为 8.322/9.063ms，pan sample interval 为 8.332/8.887ms。用户观察 detached
  图流畅但有撕裂，证明该 swap chain 的 `tearing_at_target_rate` 路径在本机也能维持约
  120Hz；严格 p95 门禁仍轻微超限。该轮没有任何落在 pan 窗口内的应用级 pointer sample，
  但有 2009 个 ImPlot pan sample，说明 secondary HWND 输入已到达 ImGui/ImPlot，却没有进入
  目前只挂在主 HWND 的输入 telemetry；这是 per-viewport 可观测性缺口，不是无效交互。
  同时发现 detached 窗口边缘显示缩放光标但尺寸不能改变。日志中的两次
  `render_target_resize` 都属于主 swap chain，detached renderer 尚未记录 create/resize/
  Present 的 viewport identity，因此缩放缺陷与 per-viewport Present 行为仍需独立诊断，
  不能据此把 detached viewport 整体标为通过。
- `logs/specforge-profile-20260718-081314.jsonl` 是 detached resize 的单变量原生装饰 A/B。
  自描述探针记录 `viewport_no_decoration=false`，其余交互 Present 仍为
  `Present(0, DXGI_PRESENT_ALLOW_TEARING)`。Spectrum 拖出后出现 Windows 原生标题栏，
  可以调整大小、最小化和最大化，证明 detached swap chain 的 ResizeBuffers/RTV 重建链
  本身可用；原无装饰窗口不能缩放的问题位于 ImGui 手动 resize 或特定 dock 节点语义，
  不是 DXGI resize 失败。原生标题栏的关闭按钮不能关闭 Spectrum，原因也已由源码确定：
  当前 `ImGui::Begin(kMainPlotWindow)` 没有传 `p_open`，而 ImGui 只在 `p_open != nullptr`
  时消费 platform close request。回停靠需要两阶段：先用原生标题栏把 HWND 完全拖入主窗口，
  触发 viewport auto-merge 并移除原生标题栏，再拖 ImGui 标签产生 docking payload 和放置提示。
  这是 OS 原生移动与 ImGui docking 拖动分属两条交互链的直接结果。故全局启用原生装饰虽
  修复 resize，却引入无效关闭按钮和两阶段回停靠，不作为已选定的最终方案。
- `logs/specforge-profile-20260718-084448.jsonl` 回到无原生装饰版本，并将测试对象换为普通
  `Spectral Lines` panel。该 secondary viewport 曾成功调整大小和拖动一次，之后再也不能
  移动或缩放；因此问题不是 Spectrum 或中心 dock 节点特例，而是无装饰 secondary viewport
  在首次平台操作后的状态/lifecycle 故障。程序在约 105s 内仍完成 2167 帧、保留主窗口输入
  和一段 70 帧 ImPlot 交互，并正常写出 `shutdown`，可排除应用渲染循环死亡和 DXGI 崩溃。
  当前 profile 仍不记录 per-viewport flags、mouse capture、ActiveID 或 HWND 消息，尚不能在
  `NoInputs` 残留、capture/ActiveID 残留和 secondary 消息未送达之间定因。该构建实际链接
  Dear ImGui 1.92.8，不能用“缺少旧版已知修复”解释；下一轮必须加入最小、可清理的
  viewport input-state 探针。
- `logs/specforge-profile-20260718-085341.jsonl` 运行
  `[DEBUG-detached-input-state-v1]` 后，从创建起始终不能移动或缩放，边缘仍显示 resize
  光标。885 条探针中，主 secondary viewport 有 866 条且始终保持 `(884, 32)`、
  `1273 x 857`；47 帧同时检测到 ImGui/Win32 左键按下并由该 viewport HWND 持有 capture，
  两者仅 1 帧不一致。`NoInputs` 只短暂出现 36 帧，不是永久残留；28 帧已建立
  `MovingWindow=Spectrum###SpecForgeSpectrumV2`，ActiveID/capture 也能随释放清除，但 ImGui
  viewport 的位置和尺寸仍完全不变。因此可排除 secondary 消息未送达、永久 `NoInputs`、
  capture 建立失败以及渲染循环死亡；剩余边界是 ImGui 鼠标坐标/目标几何未变化，或几何在
  同帧 platform update 前后被覆盖。实际布局文件 `Data/specforge-imgui-v2.ini` 也确认当前
  floating window 是 Spectrum，Spectral Lines 仍停靠在主窗口。下一轮用三阶段 position-state
  探针比较物理光标、ImGui 光标、目标几何、ImGui viewport 和 Win32 HWND，不改变交互策略。
- `logs/specforge-profile-20260718-091057.jsonl` 运行三阶段
  `[DEBUG-detached-position-state-v2]`；用户观察仅两次成功移动。5957 条 position probe
  始终对应同一 Spectrum viewport。两段长失败操作的按下点位于窗口内容区内约 `y=356`
  和 `y=597`，物理光标与 ImGui 光标分别移动约 `766 x 435`、`740 x 466`，但 ActiveID
  为 `0xE61E302D`，从未取得窗口 MoveID，因此属于内容控件交互而不是失败的标题区拖动。
  从顶部标题区开始的有效拖动均位于相对 `y=8..18`，取得 MoveID `0x1B70BF7B` 后，计算
  target、ImGui viewport 和 Win32 HWND 坐标逐帧一致；位置从 `(884, 32)` 先后提交到
  `(972, 62)`、`(1179, 82)` 等状态。所有 after-platform-update 样本中 viewport 与 HWND
  几何完全一致，可排除平台提交间歇失效。帧 1500/1560 的高度 `857 -> 33 -> 857`
  发生在标题区快速点击期间，符合 ImGui 折叠/展开，并非平台层覆盖。故窗口移动链当前可用，
  未复现“命中标题区、光标显著移动但窗口不动”；边缘缩放仍需用 border hover/held ID
  专用探针独立验证。
- `logs/specforge-profile-20260718-092013.jsonl` 运行
  `[DEBUG-detached-resize-state-v3]`，用户观察底边出现上下箭头但高度调整失败。249 条 resize
  probe 中，底边 hover 12 帧；按下后的 4 帧均满足 `ResizeBorderHeld=3`、
  `ActiveID=resize_border_down_id=233972403`，secondary HWND 也持有 capture，证明命中、
  ActiveID 和 capture 链均正确。但这 4 帧内光标固定在 `y=937`、高度固定为 857；此后
  约 1739ms 没有渲染帧，下一帧光标已到 `y=428` 且鼠标释放、ActiveID/capture 清除，
  因而 ImGui 从未获得“底边 held 且坐标发生位移”的渲染帧。源码显示主循环会在渲染前
  持续 `PeekMessage` 直到队列为空；现有 message-wait 测试仅覆盖 sent message 和
  `WM_NCHITTEST`，没有覆盖持续 queued mouse input 的渲染公平性。下一轮以每批最多 64
  条消息后让出渲染机会做单变量 A/B；若缩放恢复，即可确认 message-pump starvation。
- `logs/specforge-profile-20260718-092856.jsonl` 运行
  `[DEBUG-detached-message-fairness-v4]`。用户连续三次缓慢往返拖动，均表现为向下无反应、
  向上仅一小段缩短。496 条 fairness probe 中没有一次触及 64-message batch limit，故上一轮
  并未实际干预问题。三段主要 held 区间分别持续约 6.10s、5.56s、5.24s，最大无帧间隔
  分别为 4982.7ms、4980.1ms、4984.3ms；间隔前后物理左键、ImGui 左键和底边 held 均仍成立，
  每次恢复帧时高度才从 `857 -> 475 -> 142 -> 56` 阶梯式缩短。这与 ImGui 默认约 5s 的设置
  保存唤醒吻合，证明 resize 状态没有丢失，缺失的是 secondary queued mouse input 对渲染的
  即时 invalidation。Microsoft 的 [Win32 hook 文档](https://learn.microsoft.com/en-us/windows/win32/winmsg/about-hooks)
  也区分了监视 sent window-procedure message
  的 `WH_CALLWNDPROC` 与监视 `GetMessage`/`PeekMessage` 所取 queued message 的
  `WH_GETMESSAGE`。当前实现只安装前者，而消息泵只对 `hwnd == nullptr` 的 thread message
  主动请求帧；因此 secondary viewport 的 queued `WM_MOUSEMOVE` 没有稳定进入
  `RequestMessageRender()`。下一轮直接让所有可使画面失效的 queued window message 请求帧，
  不再使用批次上限，作为根因 A/B。
- `logs/specforge-profile-20260718-093605.jsonl` 运行
  `[DEBUG-detached-queued-invalidation-v5]`，唯一行为变化是让所有通过
  `Win32MessageCanInvalidateRender()` 的 queued window message 请求渲染帧。用户确认 detached
  viewport 可以正常调整大小和移动。1644 条 probe 覆盖 624 种不同窗口几何；第一段底边 held
  连续 3.34s、200 个样本，高度从 56 连续变化到 1367，最大帧间隔 65.4ms。全部 681 个连续
  resize-held 间隔的 p50/p95 为 16.55/33.15ms；此前三次都稳定出现的约 4.98s 无帧空洞消失。
  后续日志还记录了宽度与位置变化，和用户视觉结果一致。该 A/B 确认根因是 secondary
  viewport 的 queued input 没有触发 render invalidation，而非 ImGui hit-test/resize、mouse
  capture、DXGI ResizeBuffers 或 renderer 提交故障。正式修复边界应是：消息泵统一处理 queued
  message invalidation；现有 `WH_CALLWNDPROC` observer 只保留为非队列 sent message 的补充；
  不通过持续渲染、轮询鼠标或原生装饰绕过问题。回归测试必须覆盖 secondary HWND queued
  mouse message 能请求帧，以及 `WM_NCHITTEST` 不产生 render feedback。
- 正式修复采用 `Win32MessageRenderObserver::ObserveQueuedMessage()` 作为窄接口：主消息泵把
  每条 `PeekMessage(..., PM_REMOVE)` 取得的 message 交给 observer，observer 复用既有
  `Win32MessageCanInvalidateRender()` 谓词和 callback；原 `WH_CALLWNDPROC` hook 继续覆盖
  非队列 sent message。TDD tracer test 先以缺少该接口的 `C2039` 进入 RED，最小实现后 queued
  `WM_MOUSEMOVE` 请求帧、queued/sent `WM_NCHITTEST` 防反馈测试全部通过。无诊断宏的官方
  `ninja-msvc-portable-debug` 构建成功，完整 CTest 18/18 通过。
- `logs/specforge-profile-20260718-095641.jsonl` 使用该正式构建完成原始场景复验；日志中的
  `package_root` 指向 `build/ninja-msvc-portable-debug`，记录 228 条输入、2641 次渲染与
  正常 `shutdown`。用户确认 panel 拖出、拖回、移动和调整大小均正常，故 queued-input
  修复的自动回归与正式构建人工验收均已闭环。
- 当前没有外接显示器、混合刷新率、多适配器或通用 VRR 显示器的实测证据。

## 实现候选的验证顺序

以下是按复杂度排列的实验假设，不是已经选定的最终实现：

1. 在创建任何 swap chain 前禁用 DRR vblank virtualization，保留 compositor clock boost，
   使用无撕裂同步 Present；验证本机 DRR 是否同时达到约 120 Hz 和无撕裂。
2. 若同步 Present 仍受错误节奏约束，验证 compositor-clock paced `Present(0, 0)` 在 HWND
   flip-model 路径上是否由 DWM 无撕裂合成并达到目标节奏。
3. 仅在 `CheckPresentDurationSupport` 和系统批准都成功时，使用 custom present duration
   adapter；duration 必须来自目标刷新率和支持查询，不得硬编码为 120 Hz。微软将
   `IDXGISwapChainMedia` 定义为桌面媒体应用的无缝自定义刷新率接口，并明确限制为内置
   面板；外接显示器会通过 `(0, 0)` support bounds 报告不支持。因此即使本机验证通过，
   该 adapter 也只能覆盖相符的内屏能力，不能作为通用外屏路径。参考：
   <https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_3/nn-dxgi1_3-idxgiswapchainmedia>。
4. 若所有已验证的无撕裂高刷 adapter 都失败，选择能够维持最高刷新率的 tearing adapter，
   并记录为 `tearing_at_target_rate`；只有该路径也不可用或不稳定时才降低刷新率。
5. 只有标准 HWND swap chain 路径被证据证明无法满足多显示目标时，才评估更深的
   DirectComposition 或 composition swapchain 方案。

每一步只改变一个呈现变量，并同时记录视觉结果、交互窗口节奏、输入到画面代理延迟、
boost 释放和空闲恢复。失败的实验必须保留结论，但临时 debug 代码应删除。

## 验证矩阵

| 场景 | 当前硬件可测 | 验收重点 | 状态 |
|---|---:|---|---|
| 内屏 DRR 自动，鼠标 pan | 是 | boost 高档、最大节奏、无撕裂、释放 | H1/H2 仅约 60Hz 无撕裂；H3 连续两次约 120Hz 无撕裂，p95 待收敛 |
| 内屏 DRR 自动，触控板 pan/zoom | 是 | 原生输入延迟、惯性、无撕裂、释放 | pan/zoom 均确认获批后约 120Hz 且无撕裂；短手势重获批 p50 约 75–82ms；锚点几何待独立验收 |
| 内屏固定 60 Hz | 是 | 无撕裂、不误报 DRR 失败 | `Present(1, 0)` 约 60Hz 且无撕裂；识别为系统模式约束，p95 待收敛 |
| 内屏固定高刷（系统提供的档位） | 是 | 跟随活动刷新率、无撕裂 | 固定 120Hz：`Present(1, 0)` 约 120Hz 且无撕裂；p95 待收敛 |
| 运行中切换自动/固定刷新率 | 是 | 策略重新感知、无 stale duration | 双向均已捕获 `WM_DISPLAYCHANGE`；自动→固定 120Hz 的同步 Present 正确跟随，固定 120Hz→自动的 custom duration 已确认 reset、重新请求和重新获批，无 stale duration；两向均无撕裂 |
| 普通、沉浸、同屏 detached viewport | 是 | 所有 swap chain 行为一致 | 普通最大化的无撕裂路径约 60Hz；普通 UI 切为 borderless fullscreen 后 duration 获批、约 120Hz 且无撕裂；普通窗口和 detached viewport 的 tearing-at-target-rate 均已确认约 120Hz、流畅但可见撕裂；detached move/resize 的 queued-input 修复已通过自动测试 18/18 与正式构建人工复验；detached 无撕裂 adapter 与 per-viewport Present telemetry 仍未验证 |
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
