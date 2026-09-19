# 呈现策略历史调查与验证记录

> 历史记录：保留原呈现策略文档中各次采样的时间、构建、结果和当时结论。文中的“当前”“下一项”等只指对应调查阶段，不作为现行操作指令。
> 当前产品合同、实现和硬件验证状态见[呈现策略](../policy.md)，独立面板缩放后续进展见[窗口缩放调查](../live-resize/README.md)。

## 按原记录顺序保留的证据

- commit `8282a95` 的 compositor-clock tick 可以在本机 DRR 交互时形成约 120 Hz 的提交节奏，
  交互路径使用 `Present(0, ALLOW_TEARING)`，真实 pan 可见稳定水平断层。它满足约 120 Hz
  节奏，但只能归类为 `tearing_at_target_rate`，不是首选的最高刷新率无撕裂状态。
- 临时 `Present(1, 0)` A/B 消除了撕裂，但约 60 Hz，只能归类为
  `reduced_rate_tear_free`；按产品优先级低于保持约 120 Hz 的 tearing 路径。
- `logs/spectiary-profile-20260718-054947.jsonl` 完成了候选 1 的有效鼠标 pan 验证：
  `DXGIDisableVBlankVirtualization()` 在创建 swap chain 前返回 `S_OK`，沉浸模式下 DWM
  从 60.000 Hz 升至 120.000 Hz，`Present` 使用 sync interval 1、flags 0，视觉无撕裂；但
  30.176s 拖动窗口内 `Present interval` p50/p95 仍为 16.660/17.704ms，ImPlot pan sample
  p50/p95 为 16.686/17.807ms，input→Present p95 为 16.448ms。由此可排除“仅禁用 vblank
  virtualization 即可使同步 Present 跟随 DRR 120 Hz”这一假设；API 成功与 DWM 升档均未
  改变该 swap chain 的约 60 Hz 实际节奏。本机结果仍归类为 `reduced_rate_tear_free`。
- `logs/spectiary-profile-20260718-060018.jsonl` 完成了候选 2 的有效鼠标 pan 验证：探针
  确认交互路径为 `Present(0, 0)`，24.210s 拖动窗口内 boost 请求返回 `S_OK` 且
  `active=true`，但 compositor tick 仅增加 1453 次（约 60.0Hz），DWM timing 始终约
  60.015Hz；`Present interval` p50/p95 为 16.664/17.536ms，ImPlot pan sample p50/p95
  为 16.672/17.460ms，input→Present p95 为 16.412ms，视觉无撕裂。由此可排除
  “compositor-clock paced `Present(0, 0)` 可在本机 DRR 下维持 120Hz”这一假设；移除
  `ALLOW_TEARING` 后 compositor clock 本身也只产生约 60Hz tick。本机结果同样归类为
  `reduced_rate_tear_free`。
- `logs/spectiary-profile-20260717-195210.jsonl` 的 custom-duration 探针成功请求并最终获批
  `83333` 个 100 ns 单位，且本机视觉观察无撕裂；该次分析没有捕获有效的 ImPlot 左键
  pan-drag 窗口，因此只能作为本机 capability 与视觉证据，不能作为完整性能验收。
- `logs/spectiary-profile-20260718-060956.jsonl` 完成了候选 3 的首个有效鼠标 pan 验证：
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
- `logs/spectiary-profile-20260718-061733.jsonl` 重复确认候选 3：48.947s boost 窗口内
  5865 次 compositor tick（约 119.82Hz），duration 在请求后约 191.6ms 获批并稳定保持；
  `Present interval` p50/p95 为 8.332/8.624ms，input→Present p95 为 8.348ms，视觉仍无
  撕裂且主观为高刷新率。按住左键期间存在 4165.7ms 无任何 pointer move 的静止段，
  该段仍完成 500 次 Present（约 120.03Hz，最大间隔 11.1ms），证明“按住但不移动”
  不会退出高刷新路径。松开后 15.172s 只有 56 次 Present、最长空档 4.988s，再次证明
  间断帧号来自预期空闲。严格 8.3333ms p95 门禁仍有轻微 MISS，因此候选已具备可重复
  功能证据，但尚未完成统计门禁与其他显示模式验证。
- `logs/spectiary-profile-20260718-063654.jsonl` 完成了候选 3 的纯触控板平移验证；日志中
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
- `logs/spectiary-profile-20260718-064412.jsonl` 以 18 段分离的单向触控板滑动复测上述
  主观差异。18 个 touchpad boost 窗口全部成功获批 `83333` duration，总窗口时间
  28.737s、3398 个 compositor tick（包含每段启动/释放边界时约 118.25Hz）；duration
  获批后的 `Present interval` p50/p95 为 8.33/9.04ms。稳定同向、非惯性 transform delta
  间隔 p50/p95 为 8.34/9.30ms，窗口内方向反转从连续来回测试的 69 次降至 8 次；61 个
  超过 20ms 的 transform 空档中有 57 个邻接近零位移，符合分段滑动的起停边界。视觉
  无撕裂，主观刷新率由“不高”改善为“还行”，支持此前感受主要来自来回换向和停速，
  而不是 renderer 回落到 60Hz。另记录 adapter acquisition latency：每段重新请求后
  duration 获批延迟 p50/p95 为 75.0/103.9ms，获批前 Present interval p50 为 16.0ms，
  获批后为 8.33ms；该过渡是独立的生命周期指标，最终策略应结构化记录。
- `logs/spectiary-profile-20260718-065113.jsonl` 完成了候选 3 的纯触控板 pinch zoom 验证：
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
- `logs/spectiary-profile-20260718-062438.jsonl` 完成了内屏固定 120Hz 的纯垂直同步基准：
  探针确认没有使用 custom present duration 或 DRR 专用呈现变量，主路径为
  `Present(1, 0)`；Windows mode 为 120.000Hz，DWM 为 120.032Hz，27.930s boost 窗口内
  compositor tick 为 3353 次（约 120.05Hz）。`Present interval` p50/p95 为
  8.325/8.992ms，ImPlot pan sample p50/p95 为 8.332/8.943ms，input→Present p95 为
  8.361ms；视觉无撕裂且主观为高刷新率。这确认固定高刷模式下普通同步 Present 已能
  跟随活动刷新率，不需要内屏专用 custom-duration adapter。功能目标已满足，但严格
  8.3333ms p95 门禁仍为 FAIL；节奏能力判定与尾部抖动验收必须分别报告。
- `logs/spectiary-profile-20260718-062957.jsonl` 完成了同一纯垂直同步路径的内屏固定
  60Hz 对照：探针仍为 `Present(1, 0)`，Windows mode 为 60.000Hz，DWM 为 60.015Hz，
  27.249s boost 窗口内 compositor tick 为 1635 次（约 60.002Hz）。`Present interval`
  p50/p95 为 16.673/17.548ms，ImPlot pan sample p50/p95 为 16.670/17.454ms，
  input→Present p95 为 16.405ms；视觉无撕裂，主观刷新率不高。该结果与固定 120Hz
  基准共同确认普通同步 Present 会正确跟随当前活动模式。固定 60Hz 是用户选择的系统
  约束，不是应用主动降级；功能目标已满足，严格 16.6667ms p95 抖动门禁仍为 FAIL。
- `logs/spectiary-profile-20260718-065840.jsonl` 在同一个纯 `Present(1, 0)` 进程中完成
  DRR 自动到固定 120Hz 的运行时切换。切换前沉浸模式主拖动窗口为 12.594s，compositor
  tick 约 60.01Hz，`Present interval` p50/p95 为 16.68/17.51ms，input→Present p95
  为 16.40ms，视觉无撕裂但流畅度一般。退出交互后 boost 已释放，随后
  `WM_DISPLAYCHANGE` 明确记录 Windows mode 从 60Hz 变为 120Hz、DWM 变为 120Hz；该
  消息没有伴随 render-target resize。切换后同一 swap chain 的 12.242s 主拖动窗口为
  120.08Hz，`Present interval` p50/p95 为 8.34/8.92ms，input→Present p95 为 8.29ms，
  视觉仍无撕裂且明显更流畅。由此确认普通同步 Present 无需重建 swap chain 即可跟随
  固定模式变化，现有 `WM_DISPLAYCHANGE` telemetry 也能捕获该外部约束变化；最终策略仍
  需在该事件后重新评估 adapter，而不能只更新日志。
- `logs/spectiary-profile-20260718-070534.jsonl` 在同一个 custom-duration 探针进程中完成
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
- `logs/spectiary-profile-20260718-071148.jsonl` 证明同一 DRR 内屏、同一 custom-duration
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
- `logs/spectiary-profile-20260718-072810.jsonl` 是上述 F10 A/B 的首次尝试，但没有形成
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
- `logs/spectiary-profile-20260718-074250.jsonl` 用 v2 的 `Ctrl+Shift+Enter` 完成了保持普通
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
- `logs/spectiary-profile-20260718-075134.jsonl` 原计划验证普通窗口的
  `tearing_at_target_rate`，但所选 `build/final-drr-verify-debug/Spectiary.exe` 实际是历史
  同步 Present 构建，不能作为 tearing adapter 证据。15.122s 拖动期间 boost 正常
  `active=true`，compositor tick 增加 1815 次（约 120.02Hz），但只有 907 个 pan sample，
  `Present interval` p50/p95 为 16.666/16.898ms，Present 调用自身 p50/p95 为
  13.882/14.761ms；这组“120Hz tick、约 60Hz frame、Present 明显阻塞”的组合证明
  实际仍是同步等待。用户观察无撕裂且流畅度一般，与该结果一致。本轮再次确认普通窗口的
  `reduced_rate_tear_free` 基准，但不能判定 tearing 降级成功或失败。后续诊断二进制必须
  在日志中自描述 sync interval、flags、tearing capability 和预期降级等级，不能依靠历史
  目录名或构建时间推断呈现策略。
- `logs/spectiary-profile-20260718-075644.jsonl` 使用自描述诊断构建完成了普通窗口的有效
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
- `logs/spectiary-profile-20260718-080234.jsonl` 将 Spectrum 拖出为同屏 detached viewport，
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
- `logs/spectiary-profile-20260718-081314.jsonl` 是 detached resize 的单变量原生装饰 A/B。
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
- `logs/spectiary-profile-20260718-084448.jsonl` 回到无原生装饰版本，并将测试对象换为普通
  `Spectral Lines` panel。该 secondary viewport 曾成功调整大小和拖动一次，之后再也不能
  移动或缩放；因此问题不是 Spectrum 或中心 dock 节点特例，而是无装饰 secondary viewport
  在首次平台操作后的状态/lifecycle 故障。程序在约 105s 内仍完成 2167 帧、保留主窗口输入
  和一段 70 帧 ImPlot 交互，并正常写出 `shutdown`，可排除应用渲染循环死亡和 DXGI 崩溃。
  当前 profile 仍不记录 per-viewport flags、mouse capture、ActiveID 或 HWND 消息，尚不能在
  `NoInputs` 残留、capture/ActiveID 残留和 secondary 消息未送达之间定因。该构建实际链接
  Dear ImGui 1.92.8，不能用“缺少旧版已知修复”解释；下一轮必须加入最小、可清理的
  viewport input-state 探针。
- `logs/spectiary-profile-20260718-085341.jsonl` 运行
  `[DEBUG-detached-input-state-v1]` 后，从创建起始终不能移动或缩放，边缘仍显示 resize
  光标。885 条探针中，主 secondary viewport 有 866 条且始终保持 `(884, 32)`、
  `1273 x 857`；47 帧同时检测到 ImGui/Win32 左键按下并由该 viewport HWND 持有 capture，
  两者仅 1 帧不一致。`NoInputs` 只短暂出现 36 帧，不是永久残留；28 帧已建立
  `MovingWindow=Spectrum###SpectrumV2`，ActiveID/capture 也能随释放清除，但 ImGui
  viewport 的位置和尺寸仍完全不变。因此可排除 secondary 消息未送达、永久 `NoInputs`、
  capture 建立失败以及渲染循环死亡；剩余边界是 ImGui 鼠标坐标/目标几何未变化，或几何在
  同帧 platform update 前后被覆盖。实际布局文件 `Data/specforge-imgui-v2.ini` 也确认当前
  floating window 是 Spectrum，Spectral Lines 仍停靠在主窗口。下一轮用三阶段 position-state
  探针比较物理光标、ImGui 光标、目标几何、ImGui viewport 和 Win32 HWND，不改变交互策略。
- `logs/spectiary-profile-20260718-091057.jsonl` 运行三阶段
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
- `logs/spectiary-profile-20260718-092013.jsonl` 运行
  `[DEBUG-detached-resize-state-v3]`，用户观察底边出现上下箭头但高度调整失败。249 条 resize
  probe 中，底边 hover 12 帧；按下后的 4 帧均满足 `ResizeBorderHeld=3`、
  `ActiveID=resize_border_down_id=233972403`，secondary HWND 也持有 capture，证明命中、
  ActiveID 和 capture 链均正确。但这 4 帧内光标固定在 `y=937`、高度固定为 857；此后
  约 1739ms 没有渲染帧，下一帧光标已到 `y=428` 且鼠标释放、ActiveID/capture 清除，
  因而 ImGui 从未获得“底边 held 且坐标发生位移”的渲染帧。源码显示主循环会在渲染前
  持续 `PeekMessage` 直到队列为空；现有 message-wait 测试仅覆盖 sent message 和
  `WM_NCHITTEST`，没有覆盖持续 queued mouse input 的渲染公平性。下一轮以每批最多 64
  条消息后让出渲染机会做单变量 A/B；若缩放恢复，即可确认 message-pump starvation。
- `logs/spectiary-profile-20260718-092856.jsonl` 运行
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
- `logs/spectiary-profile-20260718-093605.jsonl` 运行
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
- `logs/spectiary-profile-20260718-095641.jsonl` 使用该正式构建完成原始场景复验；日志中的
  `package_root` 指向 `build/ninja-msvc-portable-debug`，记录 228 条输入、2641 次渲染与
  正常 `shutdown`。用户确认 panel 拖出、拖回、移动和调整大小均正常，故 queued-input
  修复的自动回归与正式构建人工验收均已闭环。
- `logs/spectiary-profile-20260718-103522.jsonl` 运行
  `[DEBUG-drr-window-eligibility-v1]`，尝试把普通最大化、borderless 工作区和 borderless
  整屏组成阶梯 A/B。普通最大化客户区为 `(0, 40) 2880×1760`，请求期间批准值始终为 0；
  第一次切换后客户区变为 `(0, 0) 2880×1800`，159/167 个请求帧获批 `83333`；第二次切换
  后 style 和几何均未再改变，79/87 个请求帧获批。原因是自动隐藏任务栏使系统报告的
  `rcWork` 与 `rcMonitor` 同为 2880×1800，但普通最大化仍保留 40px 顶部非客户区；因此
  第一次切换同时改变 style 和客户区覆盖，第二次切换没有单变量变化，尚不能区分二者谁是
  批准条件。该轮 compositor tick 在各拖动段保持约 120Hz，但三种状态实际都只有约 20.1
  FPS：`view_update` p50/p95 为 49.036/50.193ms，而 Present 调用 p50 约 0.04ms。用户观察
  全程无撕裂且非常不流畅，与应用侧 UI 工作量瓶颈一致，不能据此否定 borderless 状态的
  duration 批准。运行时起始 source 为空，随后发生 6.633s 的阻塞式载入且 plot 数据范围
  不同于既有 V0.31 基准；下一轮必须显式传入同一 V0.31 source，并先保持客户区几何不变只
  移除 window decoration，再单独扩展到整屏。
- `logs/spectiary-profile-20260718-104601.jsonl` 使用标准
  `carbon_net_increment_loglam_V0.31_X.npy` 完成了
  `[DEBUG-drr-window-eligibility-v2]` 单变量复验。普通最大化客户区为
  `(0, 40) 2880×1760`；第一次组合键只移除 decoration，并把 borderless 窗口保持在完全相同的
  客户区矩形，因此屏幕顶部留下 40px 黑条。该状态的两段 boost 分别持续 1.69s、8.49s，
  共 643 个请求帧的 `ApprovedPresentDuration` 全部为 0，帧间隔 p50 仍约 16.6ms。第二次
  组合键保持同一 borderless style，只把客户区扩展为 `(0, 0) 2880×1800`；三段 boost 共
  678 个请求帧中 462 个获批 `83333`，三段首次获批延迟分别为 166.3ms、749.3ms、
  2824.1ms。获批前帧间隔 p50 为 16.14–16.69ms，获批后降至 8.32–8.34ms；第三次恢复
  普通最大化后批准值重新为 0。`view_update` 全程 p50/p95 仅 1.293/1.580ms，排除了 v1
  的数据载入/UI 工作量混淆。由此确认：在本机 DRR 内屏上，移除 window decoration 本身
  不足以使 custom duration 获批；客户区覆盖整个 monitor 才是本轮观察到的决定性条件。
  用户全程未见撕裂，但主观只感到一般流畅且三种状态无明显差异；这与整屏阶段批准延迟大、
  仅 68.1% 请求帧获批以及整份 49.006s 交互样本仍以约 60Hz 为主一致。该结果关闭普通窗口
  custom-duration eligibility 假设，但批准延迟仍需作为 adapter acquisition 指标持续记录。
- 标准 HWND swap chain 的普通窗口路径至此已完成候选 1–3 的证伪边界，因此开始按验证顺序
  评估 Windows 11 composition-swapchain API。一次本机只读能力探针中，使用微软要求的
  D3D11 device flags 后，`CreatePresentationFactory`、`CreatePresentationManager` 和
  `SetPreferredPresentDuration(83333, 1000)` 均返回 `S_OK`，且
  `IsPresentationSupported()` 与 `IsPresentationSupportedWithIndependentFlip()` 均为 true。
  这只证明本机具备实现下一轮 A/B 的 API/驱动前提，不证明普通窗口已经达到 120Hz 或无撕裂；
  必须继续以实际 presentation statistics、帧节奏和视觉门禁验证。
- `logs/spectiary-composition-probe-20260718-113505.jsonl` 是独立普通窗口
  composition-swapchain v1 可见性实验。探针在 30.007s 内收到 3607 个 compositor tick、
  提交 3603 帧，提交间隔 p50/p95 为 8.333/8.834ms，`Present()` 调用 p50/p95 为
  0.235/0.480ms；3603 个 present 均得到 `PresentStatus_Queued` 和递增的 composition frame
  ID，且没有 runtime failure。用户却没有看见预期的青色移动竖条，同时没有收到任何
  `CompositionFrame` 或 `IndependentFlipFrame` statistics。因此这轮只证明 API 队列和
  应用提交已达到约 120Hz，不能证明内容实际显示，更不能评价撕裂。下一版首先在 Present
  前显式 flush D3D11 immediate context，并用纯色可见性哨兵区分“visual 未覆盖”与“buffer
  内容未提交”；该探针第一条 initialize JSON 多写了一个右花括号，也需同时修正。
- `logs/spectiary-composition-probe-20260718-113928.jsonl` 运行
  `[DEBUG-composition-present-v2-flush]`。探针显式 `Flush()` 后在 30 秒内提交 3601 帧，提交
  间隔 p50/p95/p99 为 8.331/8.835/9.404ms，`Present()` 调用 p50/p95/p99 为
  0.131/0.305/0.488ms；3601 个 present 均得到 `PresentStatus_Queued`，无 runtime failure。
  窗口最大化客户区为 `1646 x 1006`（DPI 96），与 presentation buffer resize 后尺寸一致。
  但用户看到的整个客户区仍为探针特意设置的白色 HWND 背景，没有看到前两秒全屏青色哨兵、
  深色背景、移动青色条或黄色标记。因此显式提交 GPU 命令没有恢复可见性，且这批约 120Hz
  statistics 仍不能解释为画面实际显示。下一轮只给 HWND 增加
  `WS_EX_NOREDIRECTIONBITMAP`，验证传统窗口重定向表面是否阻挡本探针的 DirectComposition
  visual；若白底仍在，再依次检查 DPI awareness 与 visual/surface 绑定，不同时改变这些变量。
- `logs/spectiary-composition-probe-20260718-114538.jsonl` 运行
  `[DEBUG-composition-present-v3-no-redirection]`。加入 `WS_EX_NOREDIRECTIONBITMAP` 后，用户看到
  整个客户区透明，仍没有青色哨兵、深色背景、移动青条或黄色标记。这说明该 flag 确实移除了
  传统 HWND 重定向表面，但没有让 DirectComposition 内容出现，因而否定“白色重定向表面
  单独遮挡 visual”的假设。探针仍收到 3601 个 compositor tick 并提交 3436 帧，提交间隔
  p50/p95/p99 为 8.332/8.997/9.761ms，`Present()` p50/p95/p99 为
  0.130/0.314/0.495ms；3431 个 present 为 queued、5 个 skipped，无 runtime failure。
  下一轮保留该窗口样式，只把原先的 `SetBuffer -> render/Flush -> Present` 改为微软示例采用的
  `render/Flush -> SetBuffer -> Present`，验证 presentation buffer 同步点是否在绑定时捕获。
- `logs/spectiary-composition-probe-20260718-115344.jsonl` 运行
  `[DEBUG-composition-present-v4-render-before-bind]`。改为先 render/Flush、再 SetBuffer 后，用户
  看到的客户区仍完全透明，未出现任何颜色哨兵；因此否定 presentation buffer 绑定时捕获了
  绘制前同步状态这一假设。该轮收到 3606 个 compositor tick，提交 3601 帧且全部为 queued，
  提交间隔 p50/p95/p99 为 8.329/8.795/9.370ms，`Present()` p50/p95/p99 为
  0.141/0.364/0.475ms，无 runtime failure。下一轮在窗口显示、最大化 resize 完成后重新执行
  `IDCompositionDevice::Commit()` 并等待 `WaitForCommitCompletion()`，验证 hidden-window 阶段
  建立的 visual tree 是否未在可见 HWND 上生效；render、SetBuffer 和 Present 顺序保持不变。
- `logs/spectiary-composition-probe-20260718-120121.jsonl` 运行
  `[DEBUG-composition-present-v5-recommit-after-show]`。窗口显示和最大化 resize 后的第二次
  `Commit()` 与 `WaitForCommitCompletion()` 均返回 `S_OK`，但客户区仍完全透明，因此否定
  “visual tree 只因在 hidden HWND 阶段提交而未生效”的假设。用户约 8.9s 后提前退出；期间
  1063 帧全部 queued，提交间隔 p50/p95/p99 为 8.337/8.759/9.418ms，`Present()`
  p50/p95/p99 为 0.127/0.341/0.497ms，无 runtime failure。下一轮在创建任何 HWND 前启用
  Per-Monitor-V2 DPI awareness，并记录实际 awareness 与物理客户区几何；若仍透明，则用传统
  DirectComposition surface 对照切分 HWND visual tree 与 composition-swapchain surface 路径。
- `logs/spectiary-composition-probe-20260718-121010.jsonl` 运行
  `[DEBUG-composition-present-v6-pmv2-dpi]`。`SetProcessDpiAwarenessContext` 成功且实际线程
  awareness 确认为 Per-Monitor-V2；窗口 DPI 从此前的 96 变为 168，最大化客户区也从虚拟
  `1646 x 1006` 变为物理 `2880 x 1760`，证明该单变量确实生效。用户仍看到完全透明客户区，
  因而否定 DPI 虚拟化或 presentation buffer 尺寸空间不一致是不可见根因。约 12.6s 内收到
  1518 个 compositor tick、提交 1516 帧，其中 1514 queued、2 skipped；提交间隔
  p50/p95/p99 为 8.326/8.894/9.376ms，无 runtime failure。下一轮让同一个 HWND target 和
  root visual 临时改绑传统 DirectComposition 纯色 surface：若洋红色可见，则 visual tree
  正常而 composition-swapchain surface 路径故障；若仍透明，则故障位于 HWND target/visual
  tree 本身。该对照不用于评价刷新率或撕裂。
- `logs/spectiary-composition-probe-20260718-121505.jsonl` 运行
  `[DEBUG-composition-present-v7-classic-dcomp-baseline]`。同一 HWND、composition target、root
  visual 和 D3D11 device 改绑传统 DirectComposition surface 后，用户看到整个客户区为亮
  洋红色。baseline surface 为 `2880 x 1760`、draw offset `(0, 0)`，Commit/Wait 均为 `S_OK`；
  用户约 10.2s 后退出，后台 composition-swapchain 探针仍完成 1220 帧、1227 ticks 且无错误。
  该差分确认 HWND、`CreateTargetForHwnd`、root visual、D3D11 绘制和 DirectComposition commit
  链均正常，故障边界缩小到 composition-swapchain presentation surface 的内容/绑定状态。
  下一轮恢复 presentation surface，显式设置与当前 buffer 一致的 source rect，并设置 content
  tag 以取得 CompositionFrame statistics；tag 只增强观测，不改变画面语义。
- `logs/spectiary-composition-probe-20260718-121949.jsonl` 运行
  `[DEBUG-composition-present-v8-explicit-source-rect]`，是 composition-swapchain 普通最大化窗口
  的首次端到端成功。唯一影响可见性的变化是每次创建/resize buffer 时显式调用
  `SetSourceRect(0, 0, width, height)`；content tag 只用于 statistics 关联。用户看到前两秒淡蓝色
  哨兵和黄色中线，随后为深色背景、黄色中线与持续从左向右移动的青色柱，并确认无撕裂、流畅。
  因此此前透明窗口的根因是 presentation surface 没有显式有效 source rect，不是 HWND
  redirection、render/SetBuffer 顺序、hidden-window commit、DPI 虚拟化或 visual tree 绑定。
  16.011s 内收到 1904 个 compositor tick、提交 1874 帧；提交间隔 p50/p95/p99 为
  8.334/8.880/9.468ms，`Present()` p50/p95/p99 为 0.136/0.312/0.591ms。1874 个
  PresentStatus 中 1872 queued、2 skipped；匹配 content tag 的显示统计包含 30 个
  CompositionFrame 和 1842 个 IndependentFlipFrame，首次 independent flip 出现在启动后
  359.1ms。全部 1842 个 independent-flip 样本的实际 duration 均为 `83333`（8.3333ms），
  `displayed_time` 间隔 p50/p95/p99 为 8.331/8.332/8.333ms。由此证明本机普通最大化窗口可由
  composition-swapchain API 在约 120Hz 下真实显示且无撕裂；这仍是独立探针证据，不代表
  Spectiary 主渲染器已经采用该路径，也不代表未测试的显示器/系统环境已经兼容。
- `logs/spectiary-composition-probe-20260718-122559.jsonl` 运行
  `[DEBUG-composition-present-v9-normal-window]`，只把 v8 的最大化窗口改为普通非最大化窗口。
  客户区位于 `(276, 316)`、尺寸 `1576 x 936`，显著小于 2880×1800 内屏且未覆盖屏幕；用户
  确认看到与 v8 相同的动画窗口，但未单独重述本轮撕裂与流畅度判断。12.890s 内收到 1533
  个 compositor tick、提交 1504 帧，提交间隔 p50/p95/p99 为 8.329/8.856/9.342ms；1504
  个 PresentStatus 全部 queued。匹配 content tag 的统计包含 35 个 CompositionFrame 和 1473
  个 IndependentFlipFrame，后者 actual duration 全为 `83333`，`displayed_time` 间隔
  p50/p95/p99 仍为 8.331/8.332/8.333ms。由此确认本机 composition-swapchain 的约 120Hz
  真实显示不依赖最大化或近全屏覆盖，关闭“只是另一种全屏 eligibility 特例”的风险；下一轮
  验证连续 live resize 时 buffer 注册/释放、source rect 更新、画面连续性和 tagged statistics。
- `logs/spectiary-composition-probe-20260718-123224.jsonl` 运行
  `[DEBUG-composition-present-v10-live-resize]`。用户确认除调整窗口大小时卡顿外，画面、退出和
  resize 后状态均正常。探针在 17.559s 的 live resize 区间处理 1373 次 resize、覆盖 1317
  种客户区尺寸；每次 source rect 更新均为 `S_OK`，无 runtime failure。全程提交 3387 帧、
  收到 3604 ticks；live-resize 帧间隔 p50/p95/p99/max 为 8.347/13.526/27.813/242.047ms，
  1936 个 resize 区间 PresentStatus 中有 494 个 skipped。松开后画面恢复，但随后约 5s 内仍有
  130/601 个 status skipped，说明显示模式/队列恢复不是瞬时完成。当前每条 `WM_SIZE` 都同步
  释放并重新创建、注册三个 displayable buffer；日志尚未记录该操作耗时、buffer available
  wait 或 Win32 size-move 精确边界，不能只凭源码把卡顿归因于分配。下一轮只加窄计时：buffer
  reset/source-rect/allocation 总耗时、每帧 available-event wait，以及 `WM_ENTERSIZEMOVE`/
  `WM_EXITSIZEMOVE` 标记；不改变 resize 策略。
- `logs/spectiary-composition-probe-20260718-123857.jsonl` 运行
  `[DEBUG-composition-present-v11-resize-timing]`，用户观察与 v10 相同：live resize 时卡顿，但
  窗口尺寸始终成功改变，画面和结束后状态正常。26.957s 内完成 2999 帧、3224 ticks 和 888
  次 resize，无 runtime failure。每次同步重建三缓冲的 total p50/p95/p99/max 为
  1.691/6.914/10.932/17.840ms，其中旧 buffer reset/release 占主要成本，p50/p95/p99/max 为
  1.234/5.831/9.544/17.392ms；新 buffer allocation p50/p95/p99/max 为
  0.424/1.305/2.275/9.349ms，source rect p95 仅 0.001ms。buffer available wait 通常很短，
  p50/p95 为 0.002/0.003ms，但出现 249.8、136.4、116.6ms 尖峰；`Present()` p95 仅
  0.170ms。全程 2999 个 status 中 2729 queued、270 skipped，并在 DWM composition 与
  independent flip 间切换。由此确认卡顿来自临时探针在每条高频 `WM_SIZE` 上执行 atomic
  三缓冲 release/reallocate/register，再叠加少量旧新 buffer 交接等待，不是 resize 正确性、
  source rect 或 Present 调用失败。Microsoft 的
  [composition-swapchain resize 示例](https://learn.microsoft.com/en-us/windows/win32/comp_swapchain/comp-swapchain-examples#example-15staggered-buffer-resize-operation-for-improved-performance)
  同样指出 atomic resize 昂贵且可能产生 glitch，建议跨多个 present 逐个替换 buffer。当前将
  live-resize 流畅度记为生产集成性能债务，不继续优化临时探针；正式 adapter 应 coalesce
  `WM_SIZE` 或采用 staggered resize，并保留最终尺寸必达、旧 buffer 安全退休和统计可观测性。
- `logs/spectiary-composition-probe-20260718-125328.jsonl` 和
  `logs/spectiary-composition-probe-20260718-125923.jsonl` 运行
  `[DEBUG-composition-present-v12-refresh-state]`，首次把 Windows 活动显示路径和实际 presentation
  statistics 放在同一份证据中；第二次用于“动态 120Hz”复测。两次用户均确认普通非最大化窗口
  流畅且无撕裂，实际 IndependentFlip 节奏也均约 120Hz。第一轮 16.068s 内提交 1879 帧，间隔
  p50/p95/p99 为 8.332/8.876/9.411ms，1879 个 PresentStatus 全部 queued；1848 个匹配 tag 的
  IndependentFlipFrame actual duration 全为 `83333`，`displayed_time` 间隔 p50/p95/p99 为
  8.331/8.332/8.333ms。第二轮 14.453s 内提交 1641 帧，间隔 p50/p95/p99 为
  8.330/8.855/9.679ms；1636 queued、5 skipped，1570 个 IndependentFlipFrame 的 actual duration
  仍全为 `83333`，`displayed_time` p50/p95/p99 仍为 8.331/8.332/8.333ms。两轮探针却都误报
  `DRR configured: no` 和虚拟/物理约 60Hz，因为 QueryDisplayConfig 只传入了
  `QDC_VIRTUAL_MODE_AWARE`，漏掉 Windows 11 DRR 要求的 `QDC_VIRTUAL_REFRESH_RATE_AWARE`；因此
  path flags `9` 和由此选择的 60Hz preferred duration 不能用于判定系统是固定 60Hz，也不能作为
  正确的目标选择证据。有效结论仅是画面与 tagged display statistics 确认该 adapter 实际约 120Hz、
  流畅且无撕裂；微软也明确说明 preferred duration 是提示，系统可采用该刷新率或其倍数：
  [IPresentationManager::SetPreferredPresentDuration](https://learn.microsoft.com/en-us/windows/win32/api/presentation/nf-presentation-ipresentationmanager-setpreferredpresentduration)。
  v13 只补充 virtual-refresh-rate-aware query flag，并把查询 flags 写入日志；呈现路径保持不变。
- `logs/spectiary-composition-probe-20260718-130535.jsonl` 运行
  `[DEBUG-composition-present-v13-drr-query]`，用户确认动态 120Hz 下普通非最大化窗口流畅且无撕裂。
  查询 flags 为 `82`（only-active + virtual-mode-aware + virtual-refresh-rate-aware），返回 path flags
  `25`（active + virtual-mode support + DRR boost）；虚拟刷新率 60.0001Hz、物理刷新率 120.0002Hz，
  而 DWM 桌面读数仍为 60.0154Hz。探针由物理路径计算 preferred duration `83333`、请求约
  120.0005Hz，修复后环境分类与 Windows“动态 120Hz”设置完全一致，确认 v12 的误报只来自缺失
  query flag。9.139s 内提交 1046 帧，间隔 p50/p95/p99 为 8.327/9.089/16.202ms；1027 queued、
  19 skipped。976 个匹配 tag 的 IndependentFlipFrame actual duration 全为 `83333`，显示时间间隔
  p50/p95/p99 为 8.331/8.332/8.333ms（另有 357 个 CompositionFrame）。因此 composition adapter
  已在本机 DRR 自动模式同时通过环境识别、真实约 120Hz、流畅和无撕裂四项门禁；下一步用同一
  v13 二进制测试固定 60Hz，再测试固定 120Hz。`logs/spectiary-composition-probe-20260718-130926.jsonl`
  是 DRR 自动模式的完整 30.368s 重复运行，而非固定 60Hz：path flags 仍为 `25`，虚拟/物理仍为
  60.0001/120.0002Hz。用户再次确认流畅无撕裂；3541 帧的提交间隔 p50/p95/p99 为
  8.333/8.774/9.307ms，3539 queued、2 skipped，3465 个 IndependentFlipFrame actual duration
  全为 `83333`，显示间隔 p50/p95/p99 为 8.331/8.332/8.333ms。该重复结果提高了 DRR 基线可信度，
  但不能代替固定 60Hz 对照；后者必须以控制台 `DRR configured: no` 为环境门禁。
- `logs/spectiary-composition-probe-20260718-131805.jsonl` 使用同一 v13 二进制完成固定 60Hz
  对照。环境门禁正确：path flags `9`（无 DRR boost），虚拟/物理路径均为 60.0001Hz，DWM
  60.0154Hz，preferred duration `166666`、请求约 60.0002Hz。用户主观评价“较流畅、无撕裂”。
  21.918s 内提交 1275 帧，间隔 p50/p95/p99 为 16.678/17.670/18.312ms；1274 queued、1 skipped。
  1236 个匹配 tag 的 IndependentFlipFrame actual duration 全为 `166666`，显示间隔 p50/p95/p99
  为 16.662/16.663/16.664ms（另有 218 个 CompositionFrame）。这证实固定 60Hz 是实际显示约束，
  composition adapter 不会暗中提升到 120Hz；该场景应记录为 `system_refresh_constraint`，不是 adapter
  failure。策略仍是在系统当前允许的活动物理刷新率内选择最大无撕裂节奏，不应擅自修改用户的
  全局显示设置。下一步用同一二进制验证固定 120Hz。
- `logs/spectiary-composition-probe-20260718-132244.jsonl` 使用同一 v13 二进制完成固定 120Hz
  对照。环境门禁正确：path flags `9`（无 DRR boost），虚拟/物理路径均为 120.0002Hz，DWM
  120.0322Hz，preferred duration `83333`、请求约 120.0005Hz。用户确认流畅无撕裂。11.564s
  内提交 1334 帧，间隔 p50/p95/p99 为 8.333/8.919/9.788ms；1332 queued、2 skipped。
  1300 个匹配 tag 的 IndependentFlipFrame actual duration 全为 `83333`，显示间隔 p50/p95/p99
  为 8.331/8.332/8.333ms（另有 27 个 CompositionFrame）。至此 composition adapter 在本机
  DRR 自动、固定 60Hz、固定 120Hz 三种静态状态均正确识别活动路径、跟随系统允许的最大物理
  刷新率且无撕裂；下一项是运行中切换状态时重新感知并原地更新 preferred duration。
- `logs/spectiary-composition-probe-20260718-132921.jsonl` 运行
  `[DEBUG-composition-present-v14-runtime-refresh-switch]`，从固定 120Hz 启动，运行中依次捕获
  固定 120→DRR 120、DRR 120→固定 60、固定 60→DRR 120、DRR 120→固定 120 四次
  `WM_DISPLAYCHANGE`；最后一次用户操作或系统应用过程表现为两个分立状态，说明实现必须容忍一次
  设置流程产生多个通知和中间状态。每次通知后的立即查询与 500ms debounce 查询完全一致；四次
  `SetPreferredPresentDuration` 更新均返回 `S_OK`，duration 依次为
  `83333→83333→166666→83333→83333`，没有 stale duration 或 runtime failure。48.360s 内完成
  4736 帧，4733 queued、3 skipped；匹配 tag 的统计包含 4687 个 IndependentFlipFrame 和 54 个
  CompositionFrame。IndependentFlip actual duration 的连续序列为：`83333`×819、切换中
  `166666`×1、`83333`×1473、`166666`×855、`83333`×1539，证明实际扫描在各稳定状态正确
  跟随约 120/60/120Hz。用户全程未观察到撕裂、停顿、黑屏或动画中断。

  客观统计仍捕获到显示模式切换 handoff：四个切换点前后各 1s 的最大提交间隔分别为
  128.6/339.7/230.7/119.4ms，tagged `displayed_time` 最大间隙分别约为
  233.3/598.1/481.3/158.3ms。这些间隙发生在新 preferred duration 更新之前，属于 Windows
  显示模式重配置，而非 500ms debounce 或 manager update 阻塞；稳定后立即恢复正确节奏。正式
  实现应把它记录为 `system_mode_switch_handoff`，保留原地、幂等、可重复的环境重选和 duration
  更新；本机证据表明 immediate query 已足以读到正确状态，但 debounce 仍用于合并通知，不应
  被当作切换无间隙的保证。
- `logs/spectiary-composition-probe-20260718-133558.jsonl` 运行
  `[DEBUG-composition-present-v15-redirected-hwnd]`，只移除探针此前使用的
  `WS_EX_NOREDIRECTIONBITMAP`，保留显式 source rect、composition surface、buffer、duration 和
  present 路径不变。窗口扩展样式实际为 `256`（`WS_EX_WINDOWEDGE`），确认是普通 redirected
  HWND；用户未发现画面缺失、撕裂或流畅度异常。固定 120Hz 下运行 20.187s、提交 2381 帧，
  间隔 p50/p95/p99 为 8.328/8.989/9.696ms，2381 个 PresentStatus 全部 queued。2349 个匹配
  tag 的 IndependentFlipFrame actual duration 全为 `83333`，显示间隔 p50/p95/p99 为
  8.331/8.332/8.333ms（另有 36 个 CompositionFrame）。因此早期白屏是缺失 source rect 所致，
  不是普通 HWND redirection 与 composition swapchain 不兼容；正式主窗口和 ImGui detached
  viewport 无需修改扩展窗口样式即可集成 adapter。
- `logs/spectiary-profile-20260718-141859.jsonl` 是 Composition 正式接入生产 renderer 后的首次
  主窗口、沉浸模式和 detached viewport 联合交互验收。环境门禁为 DRR 自动：virtual/physical
  约 60/120Hz、请求约 120Hz，主窗口与 detached viewport 均选择 `composition`，没有 backend
  fallback 或 degradation；全部 drag 窗口内 feedback 的最后实际 IndependentFlip duration 均为
  `83333`，没有 `166666` 样本。用户确认三种 Plot 均未见撕裂或功能异常，detached 移动、调整
  大小和拖回均成功；仅 live resize 仍有已知卡顿但没有 resize failure。37.272s 聚合拖动中，
  pan sample interval p50/p95 为 8.326/8.735ms，Present interval 为 8.322/8.855ms，
  input→Present p95 为 8.344ms；因此功能与视觉门禁通过，但严格 8.3333ms p95 总门禁仍为 FAIL。
  全程 main feedback 聚合 10678 submissions、10569 IndependentFlip、32 skipped、0 buffer acquire
  skip；detached 聚合 3731 submissions、3045 IndependentFlip、250 skipped、357 次非阻塞 buffer
  acquire skip。后者证明慢/resize 中的 viewport 没有阻塞主窗口，但也量化了 atomic 三缓冲 resize
  的现有流畅度债务。
- 同一生产日志还确认“已开始 Plot drag 后按住左键但不移动”会继续空提交：最长 3181.96ms
  静止段中没有 pointer move 或 axis change，但完成 380 次 Present（119.42Hz）；松开时
  `plot_interaction_active` 变为 false 且 compositor boost 立即释放。根因是
  `IsPlotPanDragActive` 按“从 drag 开始直到按钮释放”定义 latency-sensitive interaction，
  compositor tick 因而持续请求帧；不是 Composition 自激。该行为减少暂停后恢复移动时的升档风险，
  但不符合最严格的“无画面更新不提交空帧”解释，需在提交前明确选择是否把 tick permission 与
  render invalidation 解耦。后续实现将 compositor tick 降为 render permission，不再自行请求帧；
  `logs/spectiary-profile-20260718-143835.jsonl` 中静止段出现约 1.318s 的 Present 空洞，用户确认
  鼠标按住静止时不再持续刷新，而继续移动仍正常。
- Precision Touchpad 路径不能仅沿用 mouse drag 状态：`logs/spectiary-profile-20260718-160236.jsonl`
  的定向探针在 touchpad active 期间记录 2440 轮 invalidation，其中 1850 轮最后来源是同一主
  HWND 上的 queued message `150`（`0x0096`），581 轮是专用 gesture wake `32851`
  （`WM_APP+0x53`），跨线程 sent message 只有 1 轮。SDK 10.0.26100 没有公开 `0x0096`
  常量；它由 Direct Manipulation manual-update context 的 subclass 消费。正式策略仍 dispatch
  该消息，但只在相同 HWND 已挂载该 context 时把它分类为 `PumpUpdates`，不把它作为 render
  invalidation；同号消息在其他 HWND 及所有普通 queued pointer input 维持原行为。
- 两个失败实验保留为时序边界证据：把 `IDirectManipulationUpdateManager::Update()` 从 plot
  `Poll()` 移到 compositor tick WndProc/独立主循环后，`152837` 和 `154533` 分别只有 54/44 个
  zoom、没有 pan，用户确认画面不响应双指 pan；完全忽略 `0x0096` 后，`160826` 虽能在静止时
  停帧，但同一次接触恢复移动不能可靠响应。最终实现保留原 `Poll()->Update()`，并把 `0x0096`
  作为“只推进 input、不直接渲染”的 demand；一个消息批次只合并一次 demand，每个 compositor tick
  最多执行一次 standalone pump。gesture wake 随后渲染时，`Poll()` 仍可在同一 tick 再调用一次
  `Update()`；这不是调用次数不变量，下面的实机 A/B 证明不能安全地强制合并成一次。
- `logs/spectiary-profile-20260718-161436.jsonl` 暴露了未按 tick 限速的中间实现：4718 帧内记录
  161046 个 gesture，其中 157343 个为 inertia，多个微小增量批量落到同一帧，用户观察到惯性
  卡顿。最终 `162048` 将 1990 个 gesture 一一对应到 1990 个独立帧，该日志的 p95/max 每帧均为 1；
  接触阶段 gesture interval p50/p95 为 8.33/9.44ms，惯性为 8.33/8.94ms。用户按“移动—不抬手
  静止—恢复移动—松手惯性”复验，确认未发现异常；这闭环了静止零空提交、同接触恢复响应和
  约 120Hz 惯性三项本机 DRR 验收。外接/混合刷新率与 clock 不可用 fallback 仍需后续硬件验证。
- 自动回归另覆盖两个失效边沿：compositor waiter 异常退出后，最后一个 tick 在 inactive clock
  状态下请求且只请求过渡帧，由 `CompleteFrame()` 建立 9ms 触控板 fallback deadline；正常 active tick
  仍不主动 invalidation；该边沿写入 `waiter_failure_fallback` 及 wait result。专用 gesture wake 的
  `PostMessageW` 若失败，会在持锁状态回滚
  `wake_pending`，后续增量可重试，避免 coalescing 永久锁死。
- 强制 single-update 的两轮失败实验否定了“让 pump 后的 `Poll()` 只 drain”方案。
  `logs/spectiary-profile-20260718-165754.jsonl` 的第一版 boolean gate 只有 734 个 gesture（其中 zoom 8）、
  1075 次 Present 和 26 次 boost 切换；用户观察到 pinch 巨卡、停速后同接触恢复不响应。第二版在
  新 tick 重置 gate，`171212` 的 zoom 恢复到 271，但总 gesture 仅 649、inertia 仅 16、boost 切换
  增至 36 次，用户确认两个问题依旧。两次都没有 waiter failure、backend degradation 或单帧多 gesture；
  唯一生效变量是跳过 `Poll()->Update()`。因此移除 gate，恢复经 `162048` 验证的 standalone pump 加
  `Poll()->Update()` 路径；1990 gesture 对应 1990 frame 仅作为该次实测，不再写成结构性调用次数保证。
- `logs/spectiary-profile-20260718-172023.jsonl` 完成撤销 single-update gate 的恢复 A/B，用户按连续
  pinch、静止不抬手、同接触继续移动及松手惯性复验，确认未发现异常。日志记录 1869 个 gesture：
  1143 pan、726 zoom、247 inertia，分别落在 1869 个独立帧且无单帧堆积；input→gesture p50/p95
  为 6.593/7.371ms，input→Present 为 8.183/9.319ms，惯性 interval 为 8.319/8.975ms。全程
  boost 切换 12 次，无 waiter failure 或 presentation degradation。该结果恢复了 `162048` 的正常
  交互形态，并确认 reviewer P2 应通过纠正文档保证范围解决，而不是强制 single-update。
- 当前没有外接显示器、混合刷新率、多适配器或通用 VRR 显示器的实测证据。
