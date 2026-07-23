# SpecForge 响应速度测试流程

## 目标

这套流程专门用于判断主图 ImPlot pan/drag 是否跟手。默认场景是：

- 主窗口处于正常桌面大小。
- 主图窗口为 `Spectrum`。
- 用户在主图 plot 区域内按住左键拖拽平移。
- 不混入 wheel zoom、docking 调整、侧栏操作或窗口 resize。

结论必须来自新生成的 JSONL profile，不用旧日志或体感替代。

实现层面的响应性约束、已发生的退化复盘和后续设计规则记录在
[UI 响应速度实现约束与复盘](ui_responsiveness.md)。

## 指标口径

采集时同时记录四类事件：

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
- `compositor_clock`: 初始化与 boost 状态变化。`available` 表示 Windows API 可用，`requested` 表示交互策略提出请求，`active` 表示请求成功且 compositor tick pacing 正在运行。

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

程序启动后只做一件事：在 `Spectrum` 主图 plot 区域按住左键连续平移 10-15 秒，然后关闭程序。脚本会等待 SpecForge 退出，再分析本次运行生成的 `logs/specforge-profile-*.jsonl`。

自动化采集继续使用 `SPECFORGE_PROFILE=1`，以便从进程启动阶段保留完整上下文。Release 版本也可以通过
`Settings > Diagnostics` 在运行时开始/停止采集；沉浸模式右上角的 `REC` 标记表示正在录制。复现卡顿后
尽快停止录制，分析时结合停止前的一段帧时间线和输入事件定位。Portable build 的默认输出目录是可执行
文件旁的 `Data/logs/`，用户可在 Diagnostics 设置中修改；性能脚本会显式设置
`SPECFORGE_PROFILE_DIR`，覆盖 UI 设置并把本次分析日志重定向到仓库 `logs/`，避免和 portable 包内状态
混在一起。

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

## 显式添加/打开数据源延迟

录制开启时，File 菜单或 Files 面板的文件/文件夹选择结果一经 `OpenSource` 接受，就会创建独立的
`source_load_latency` trace。命令行启动参数发生在运行时 recorder 状态进入 Shell UI 之前，不属于这条
“用户接受选择结果”的口径。trace 复用 source load queue 的阶段打点，但不会伪装成 Previous/Next
navigation。

`outcome=presented` 的终点与导航 trace 相同：最终目标 snapshot 已激活、UI 已更新，并且该精确 snapshot
已向 Spectrum viewport 提交 draw，随后该 viewport 完成第一次成功 `Present`。只完成 decode 或
`result.loaded` 不会提前结束 trace；失败、拒绝和被更新意图替代分别记录为 `failed`、
`rejected`、`superseded`。

每条 trace 会先写逐层明细，最后写汇总：

- `source_load_latency_preparation_round`：每次 source inspection/decode/context/revalidation
  尝试；同一 load attempt 内发生源文件变动并重试时会有多个连续 round。
- `source_load_latency_attempt`：一次 queue task 的完整排队、worker、准备、发布和 UI drain
  阶段；follow-up/retarget 会保留多个连续 attempt。
- `source_load_latency`：一次显式添加意图的终态汇总。

单条 `source_load_latency` 汇总事件提供：

- `source_load_id`、`first_source_task_id`、`final_source_task_id`、`target_index`、
  `attempt_count`：关联一次显式添加及其可能的 follow-up/retarget。
- `request_kind=explicit_open`、`source_kind=file|folder`、`context_reused`、
  `workflow_reused`、`preparation_round_count`、`listing_scan_count`：描述请求和加载路径。
- `accept_to_enqueue_ms`、`queue_wait_ms`、`source_inspection_ms`、`decode_ms`、
  `context_prepare_ms`、`source_revalidation_ms`、`workflow_prepare_ms`、
  `completion_ready_ms`、`ordered_publish_wait_ms`、`completion_service_wait_ms`、
  `retarget_gap_ms`、`activation_ms`、`ui_update_ms`、`ui_to_present_ms`：完整阶段拆分。
- `total_ms`：选择结果被接受到首次成功 `Present`；非 presented 结果则到其终态。
- `accepted_steady_ns`、`first_enqueued_steady_ns`、`final_completion_drained_steady_ns`、
  `snapshot_activated_steady_ns`、`ui_updated_steady_ns`、`first_present_steady_ns`、
  `terminal_steady_ns`、`activation_frame`、`presentation_viewport_id`：校验单调时间和精确
  viewport 归属。路径本身不写入 profile。

定量回归必须使用严格 analyzer，而不是只读取汇总字段：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass `
  -File scripts\analyze-source-load-profile.ps1 `
  logs\specforge-profile-<timestamp>.jsonl
```

analyzer 会按 `source_load_id` 关联 summary、attempt 和 preparation round，验证连续索引、时间
单调性、阶段时长、attempt/round 聚合、retarget gap、最终 target/task/source 状态、精确
Present 终点，以及末尾 `profile_recorder_summary` 和 `dropped_events == 0`。默认任何不完整或
不一致都会返回非零；仅做事故日志诊断时可加 `-ReportOnly`，它会输出完整前缀的统计并同时列出
严格校验失败，不可作为回归 PASS。

## 上一条/下一条导航延迟

运行时录制会为显式标记来源、并且真正触发异步 source load 的上一条/下一条操作写
`navigation_latency` 事件。来源不是根据通用 `navigation.moved` 结果反推，而是由具体
command call site 传入：左右键分别为 `keyboard_previous` / `keyboard_next`，上一条/下一条
按钮分别为 `ui_previous` / `ui_next`，标签 workflow 自动推进为 `auto_advance`。`LocateRow`、
名称搜索跳转和其他通用导航命令不会混入这些统计。没有移动、没有目标或不需要异步加载
的命令也不会伪装成一次 spectrum switch。

键盘起点是应用消息循环从任意 SpecForge HWND（包括 detached viewport）取出初次
`WM_KEYDOWN` 的时间。只有 ImGui shortcut router 在同一帧实际接受对应的左右键后才消费
该候选；同方向多次输入取最新边沿，未消费的按键在帧末清除。这样被文本框、popup、修饰键
或其他路由拦截的方向键不会污染下一次导航。若缺少可关联的原始键盘边沿，本次键盘导航
不会降级成较晚的 UI command 起点。

UI 上一条/下一条的 `input_steady_ns` 定义为 `ImGui::Button()` 接受点击后、调用 session
command 前的命令接受时间，不是原始鼠标硬件或 `WM_*BUTTON*` 消息时间。因此这两类样本的
`input_to_request_ms` 预期接近 0，衡量的是“UI 命令接受到首次成功 Present”；不能据此宣称
测得了鼠标按下到显示的端到端延迟。若后续需要真实鼠标口径，必须另行实现按钮 ID 与原始
鼠标边沿的显式关联，不能把当前字段重新解释为设备输入时间。workflow `auto_advance` 同样以
workflow 接受并提交推进命令的时刻为起点。

`outcome=presented` 表示目标 snapshot 已激活、UI 已消费新状态，并且该精确 snapshot handle 已实际向
某帧、某 viewport 提交 Spectrum plot draw，随后该 viewport 完成第一次成功 `Present`。折叠/裁剪导致
`ImGui::Begin()` 或 `ImPlot::BeginPlot()` 不接受绘制时不会产生提交凭据。主窗口和 detached viewport
renderer 都提供带 viewport ID 的成功时间；其他 viewport、其他 snapshot、未提交 Spectrum draw 的帧或
失败的 Present 都不会完成 trace。
同一个 Spectrum viewport 同一时刻只有当前已激活 snapshot 的 trace 可以消费成功 Present；
新的 snapshot 激活会将仍未成功展示的前一条记为 `superseded`，不会让一次 Present 同时完成
多个 trace。连续快速翻页时，被更新意图替代的请求会记录为 `superseded`；其余终态还包括 `failed`、
`rejected` 和 `coalesced`。因此分析时应同时看 outcome 数量，不能只保留最快的成功样本。

每次 load/retarget 会先写一条关联同一 `navigation_id` 的
`navigation_latency_attempt`，保留该次 load 自己的 target、task ID、全部阶段时间和
`attempt_total_ms`。同一 worker 因 source/companion TOCTOU 变化而内部重试时，每轮另写
`navigation_latency_preparation_round`，包含该轮 inspection、decode、context、revalidation
时间和最终是否通过 revalidation；attempt 与最终汇总的这四段时间均为各轮之和，不会用最后
一轮覆盖前一轮。最终的 `navigation_latency` 汇总事件提供：

- `total_ms`: 输入到首次成功 `Present`；非 presented 结果则到其终态。
- `target_resolution_ms`、`enqueue_ms`、`queue_wait_ms`: 主线程求目标、入队和等待 worker。
  `target_resolution_ms` 进一步严格拆成六项，且六项之和必须等于 aggregate：
  `effective_index_ms`（读取 committed/pending effective index）、
  `pending_activation_supersede_ms`（检查和更新既有 pending activation/follow-up 的
  supersede/cancel bookkeeping）、`base_sequence_ms`（读取或必要时构造 sequence state，
  并投影导航起点 cursor）、
  `target_lookup_ms`（按 previous/next/label/locate 规则求目标）、
  `target_sequence_ms`（把同一 sequence state 投影到目标 cursor）和
  `navigation_state_result_ms`（其余 navigation state mutation、result propagation 与
  owner-boundary orchestration）。最后一项是从完整 `target_resolution_ms` 扣除前五项所得，
  因而保留边界调用、轻量 bookkeeping 和插桩本身的残余，不应解释成某个隐藏 cache。
- `source_inspection_ms`、`decode_ms`、`context_prepare_ms`、`source_revalidation_ms`、
  `workflow_prepare_ms`: 后台 source 检查、光谱解码、collection context、TOCTOU revalidation
  和 workflow 准备。
- `completion_ready_ms`、`ordered_publish_wait_ms`、`completion_service_wait_ms`: worker
  准备完成后的收尾、等待有序发布，以及完成已发布到主线程真正 drain 的时间。
- `retarget_gap_ms`: 多段 follow-up 之间的主线程间隔；其余 worker 阶段是所有 attempt 的
  分段用时之和，不会用最后一跳覆盖前一跳。
- `activation_ms`、`ui_update_ms`、`ui_to_present_ms`: 主线程激活 snapshot、更新 UI 和提交
  首个可见代理帧。
- 汇总事件的 `from_index`、最终 `target_index`、`input_kind`、`attempt_count`、
  `presentation_viewport_id` 和 `cache_hit`，以及 attempt 事件的 `target_index`、
  `source_task_id`、`source_kind`、`workflow_reused`、`context_reused`：用于关联请求性质。
  每个 preparation round 也有自己的 `context_reused`，因此 TOCTOU retry 不会覆盖前一轮。
  `cache_hit=true` 只表示最终 worker attempt 复用了 roster 中已经成功解码、验证并激活过的
  immutable snapshot，且本轮没有调用 file/folder decoder。候选 snapshot 仍必须在后台
  worker 对 source/companion/annotation dependency 和 folder generation（若适用）执行
  pre/post currentness 检查；证明失败会回退 decode，并保持 `cache_hit=false`。命中时
  `decode_ms` 只包围已有 handle 的验证，正常应接近 0；单纯的 listing、context 或 workflow
  reuse 不算 snapshot cache hit。
- target-resolution 诊断还记录 `row_count`、`filter_active`、`sort_active`、
  `query_active`、`pending_present`、`sequence_cache_hit` 和
  `sequence_build_count`。这里 `pending_present` 是 command 进入 Navigation 时是否已有
  deferred sample target；`sequence_cache_hit` 只表示本次 target resolution 是否真的读取
  已 materialize 的 sequence cache，不能从“owner 存在 cache”推断为 `true`；
  `sequence_build_count` 是本次实际构造次数。sequence state 缓存排序拓扑、active membership、
  source-row position 和 query matches；committed/pending/base/target index 只做 O(1) cursor
  投影，不使 state 失效。query、filter、sort 或 source/context 输入改变时完整失效并由 owner
  重建；label advance 的 eligibility 仍在每次 `target_lookup_ms` 内按需计算，不进入缓存。
  因此 warm previous/next 应记录 `true / 0`；真正 cold 的首次 target resolution 最多构造一次，
  记录 `false / 1`。

Folder source 的 warm navigation 会复用上一次验证通过的 immutable listing generation。
`source_inspection_ms` 正常只包含窄的目标文件/依赖检查；若目录 generation 未失效，
`source_revalidation_ms` 也只包含 generation poll 与 source/annotation dependency stat，
不再执行 full folder scan、sort 或 listing compare。首次打开、stale target、目录变化
重试，或系统无法建立 directory-change generation 时，full scan 可以重新出现。
这个 listing generation cache 自身不是 snapshot cache hit；只有同一验证边界下的 resident
snapshot 真正跳过 decoder 时才把 `cache_hit` 写为 `true`。

Known source 还会保留上一次完整准备并通过 post-decode 检查的 context reuse proof。
当 source/companion/annotation dependency state、decoded count/path 和 folder generation
（若适用）均未变化时，`context_reused=true`，worker 不再读取完整 sample-name/annotation
manifest，也不再遍历 folder listing 重建 identity/sample names。任一证明条件不成立时
`context_reused=false` 并走原完整 context materialization；`workflow_reused=true` 仍只表示
最终 identity 可沿用既有 workflow，两者不能混为同一字段。

Roster 还维护 raw row index 驱动的 resident snapshot history。每个 source 继续保留最后一个
snapshot 以维持 source reactivation 语义，额外的非当前 snapshot 使用跨 source LRU，全局最多
8 个且估算的 x/y payload 总量不超过 128 MiB；超限项通过现有 background reclaimer 释放。
键包含 source identity/fingerprint、context/dependency proof、folder generation handle
（若适用）和 raw row index。query/filter/sort 只改变访问顺序，不复制 snapshot；source、
文件内容、annotation/context 或 directory generation 变化会 miss 并在新结果提交时清空旧
边界。前台 Previous/Next/auto-advance 成功激活后，会按同一 filtered/sorted sequence 和当前
方向只选择下一条 raw row 做低优先级预取。预取使用一个可取消的 unordered publication
任务，不经过 `EnqueueBatch`，也不会阻塞之后的前台有序完成；任何新输入、反向、source/query/
filter/sort/context 变化都会取消或使旧结果失效。worker 仍执行 source/context/generation
pre/post 验证，只允许在已知 context 可复用时发布 immutable snapshot；UI drain 只把它加入同一
roster residency，不激活 snapshot、不移动 committed/pending index，也不提交 workflow。

`navigation_latency.cache_kind` 区分 `none`、`history` 和 `prefetch`；`cache_hit` 保留为兼容
布尔字段，并且必须与 `cache_kind` 一致。每个 speculative task 另写 `navigation_prefetch`
事件，包含 `prefetch_id`、`source_task_id`、raw `target_index`、`direction`、开始/终止时间和
`completed`、`canceled`、`stale`、`failed` 或 `consumed` outcome。同一录制内，`consumed`
只允许跟在同一 prefetch 的 `completed` 之后，且 task/raw row/direction/scheduled time 必须
逐字段一致，消费时间不得早于完成时间；若录制开始前预取已经完成，也允许仅出现一个
`consumed` carry-in。`canceled` 的 `cancel_requested_steady_ns` 是 UI 发出取消的时间，
`terminal_steady_ns` 是 worker 结束 speculative work、在 queue publication boundary 发布
terminal marker 的时间。分析器会按 cache kind
和 prefetch outcome 汇总；第二次连续同方向
导航若消费成功，应看到 `cache_kind=prefetch` 且 demand attempt 的 `decode_ms` 接近 0。

每条新的 folder `navigation_latency_preparation_round` 还提供三项失效诊断：

- `hint_present`：本轮开始时是否持有可复用的 listing generation；
- `generation_current_at_start`：该 hint 的目录变更 token 在本轮开始时是否仍有效；
- `listing_scan_performed`：本轮是否执行过完整目录枚举、排序和 fingerprint 构建。

分析器会按这三个字段分组报告 folder inspection 的 p50/p95。旧 profile 没有这些字段时
仍可分析；一旦 profile 中出现任一新字段，所有 preparation round 都必须提供合法的
JSON boolean。

分析器也会按 `source_kind × context_reused` 报告 context prepare 的 count、p50 和 p95。
旧 profile 没有 `context_reused` 时仍可分析；新 profile 中 attempt 与 preparation round
必须提供合法 JSON boolean，且 attempt 值必须等于最终 preparation round。

旧 navigation profile 没有 target-resolution 子阶段和上述诊断字段时仍按原 aggregate
schema 分析。一旦同一 profile 的任一 `navigation_latency` 事件出现任一新字段，所有
`navigation_latency` 事件都必须提供完整字段集、合法的 JSON boolean/integer/non-negative
duration，且六项 duration 之和必须与 `target_resolution_ms` 在 0.001ms 容差内一致。这样旧证据
可继续读取，同时不允许部分升级的新日志被当成完整采集。

采集时先在 `Settings > Diagnostics` 开始录制，用真实数据连续执行若干次上一条/
下一条，等最后一条显示后再停止录制。然后运行：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\analyze-navigation-profile.ps1 logs\specforge-profile-YYYYMMDD-HHMMSS-mmm.jsonl
```

比较 UI 与键盘 target resolution 时必须固定同一 source、同一 filter/sort/query 状态和同一
source-row 区间，分开录制纯 UI 与纯键盘各至少 100 次成功 presented navigation。每次输入要等
目标显示后再继续，除非实验明确要比较 rapid-navigation pending path；普通输入对比要求
`pending_present=false`。记录 source、起止 index、方向、样本数和 profile 路径，不能把不同
目录、不同 index window 或 Previous/Next 混成输入设备差异。

`specforge_shell_source_load_activation_tests` 还在同一 synthetic source 的固定 `0 ↔ 1`
窗口，对 UI/keyboard 的 Previous/Next 各执行 100 次 presented 复测，并输出
`base_sequence_ns`/`target_sequence_ns` 的 p50/p95。它锁定 accepted-command 之后的 input kind、
cache/build 计数、load/activation/Present 行为和投影成本；它不模拟物理点击/按键，也不替代真实
Portable source 的 JSONL A/B。真实 IO/decode/context/renderer phase 是否无回退，仍以随后实际
数据采集为准。

预取 A/B 必须在同一 active source、同一 query/filter/sort/context 和同一 index window 下，
将连续 Next 与反向 Previous 分开各录制至少 100 次。最终并列记录 snapshot hit rate、
`total_ms` p95、`decode_ms` p95、`activation_ms` p95，以及 `queue_wait_ms`/
`completion_service_wait_ms` p95；同时确认
`sequence_cache_hit=true/false` 两组仍可解释、稳定 folder 为 `listing_scan_performed=false`
且 `context_reused=true`。这些数字只能来自对应的完整 JSONL，不能用 synthetic test 时间估算。

Portable build 的实际日志通常在 `Data\logs\`，也可以把对应完整路径传给脚本。分析器会
校验唯一且位于末尾的 recorder summary、`dropped_events == 0`、至少一条导航事件和至少
一次成功 presented。每条成功记录还必须具有完整 ID、有限非负 duration、连续 attempt、
单调时间戳和与时间戳一致的阶段用时，否则即使存在 `outcome=presented` 也会 FAIL。
有效样本按 `input_kind` 分组报告各阶段的 p50/p95/max，键盘输入起点不会和 UI command
起点混算。percentile、JSONL 读取和 recorder summary 校验与通用 analyzer 共享同一实现，
其中 `dropped_events` 必须是非负 JSON 整数，布尔值或小数即使可被 PowerShell 转换成 0
也会 FAIL。p95 使用相同的线性插值。只查看、不让完整性问题返回非零时加 `-ReportOnly`；
遇到非法或截断 JSONL 时，它会在首个坏记录处停止、报告此前完整样本与解析错误，并以 0
退出。默认门禁模式仍立即以非零退出，不会把可解析前缀视为完整录制。

这项插桩在热路径创建一个小型 trace 和每段 load attempt、写入原子时间点，并在终态向
现有异步 profile 队列提交 attempt 与汇总 JSON；不会在输入、worker 或 UI 帧同步写磁盘。
它仍有非零但预期很小的成本，严格评估时应在相同数据和操作下做 recorder on/off A/B。
`Present` 完成是对应 Spectrum viewport 的应用提交完成代理，不是屏幕扫描或光子延迟。

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
powershell -ExecutionPolicy Bypass -File scripts\analyze-profile.ps1 logs\specforge-profile-YYYYMMDD-HHMMSS-mmm.jsonl
```

手动只看报告：

```powershell
powershell -ExecutionPolicy Bypass -File scripts\analyze-profile.ps1 logs\specforge-profile-YYYYMMDD-HHMMSS-mmm.jsonl -ReportOnly
```

144Hz stretch 预算：

```powershell
powershell -ExecutionPolicy Bypass -File scripts\analyze-profile.ps1 logs\specforge-profile-YYYYMMDD-HHMMSS-mmm.jsonl -BudgetMs 6.9444
```

120Hz 预算：

```powershell
powershell -ExecutionPolicy Bypass -File scripts\analyze-profile.ps1 logs\specforge-profile-YYYYMMDD-HHMMSS-mmm.jsonl -BudgetMs 8.3333
```

## DRR boost 验证

> 本节记录 commit `8282a95` 的已实现路径和历史 profile 结果，不代表最终产品呈现合同。
> 该路径的 `ALLOW_TEARING` 已由真实 pan 的稳定水平断层证明存在可见撕裂，因此只能作为
> “保持最高刷新率、允许撕裂”的第一层降级，不能作为首选状态。后续方案、降级顺序、
> 可观测性字段和待补硬件矩阵以
> [显示呈现产品合同与验证矩阵](presentation_policy.md)为准。

Windows 11 DRR 模式下，主图左键 pan/drag 与命中主图的 Precision Touchpad manipulation
会请求 compositor clock boost。交互期间由独立 compositor-clock waiter 把真实 clock tick
投递给 UI 主循环。DXGI 支持 variable-refresh presentation 时，主 swap chain 和 detached
viewport swap chain 使用 capability-gated `ALLOW_TEARING` flip-model 路径；主窗口按
`Present(0)` 提交，避免再次等待被虚拟化的 DXGI vblank。输入消息只积累下一帧状态，不能
绕过 compositor tick 触发额外渲染。交互结束后释放 boost 并恢复主窗口 `Present(1)`。
waiter 只投递合并后的唤醒消息，不在后台线程访问 ImGui、ImPlot 或 D3D11 对象。clock
pacing 活跃时不并行安排 timer frame；只有 API 不可用、boost 失败或 waiter 退出后，
触控板连续更新才使用 9ms fallback deadline。

验证时必须同时满足：

1. `compositor_clock` 初始化事件为 `available=true`。
2. 交互开始/结束分别出现 `requested=true/false`，开始事件为 `active=true`。
3. DRR 环境下交互窗口的 `present interval p50` 接近 8.33ms，而非 16.67ms。
4. `input -> present p95` 按同一份日志通过目标预算。
5. 交互结束事件为 `requested=false`、`active=false`、结果为 `S_OK`；下一次 boost 前
   `tick_count` 不应增长。若要证明系统最终回到基础刷新率，还应在释放后采集 DWM timing，
   或确认下一次启动的 `display_environment` 已回到基础频率。

`active=true` 不是系统实际升频的充分证据；面板、驱动、供电策略或系统设置仍可能阻止
DRR 升档。Windows 10 或 API 缺失时会自动保留原有 display-vsync 路径，不能把这种降级
记录为 DRR 失败。最小复测矩阵仍应包含 DRR/固定 120Hz × 鼠标/触控板，并用同一份真实
数据、同一交互窗口和同一统计口径比较。

### 2026-07-17 本机 DRR 验证

真实数据 `carbon_net_increment_loglam_V0.31_X.npy` 上：

- 鼠标日志 `logs/specforge-profile-20260717-184143.jsonl`：DWM 120.000Hz，Present interval
  p50/p95 为 8.317/9.140ms，input→Present p95 为 8.397ms；相对实现前 DRR 的
  16.881ms 改善约 50.3%，与固定 120Hz 的 8.417ms 基本一致。
- 触控板日志 `logs/specforge-profile-20260717-184604.jsonl`：两个 boost 窗口的 clock
  为 119.966/119.868Hz，合并 Present interval p50/p95 为 8.324/9.322ms，原生输入时间
  →Present p95 为 9.254ms；相对实现前 DRR 的 16.983ms 改善约 45.5%。
- 两次触控板释放均为 `S_OK` 且 `active=false`；释放间隔内 `tick_count` 保持 1860，
  最终释放后 14.404s 才退出且无 `shutdown_release`。该次启动的 DWM timing 为
  60.017Hz，也证明上一轮已最终回落到基础频率。

这些结果应使用 `BudgetMs 8.3333` 解读；脚本默认的 7.6923ms 是独立的 130Hz 验收线。
严格的 120Hz `input→Present p95` 门禁仍未完全通过：鼠标超出 0.064ms，触控板超出
0.921ms。因此当前结论限定为 clock 与 p50 提交节奏达到约 120Hz、可见代理延迟接近减半，
而不是严格 p95 门禁已通过。Present 完成事件也不是直接的 scan-out/光学延迟测量。

## 报告格式

报告结论按这个顺序写：

1. 数据源：日志文件名、窗口/显示器刷新率、预算。
2. 场景：只包含 ImPlot 主图左键 pan/drag，还是混入了其他操作。
3. 实际环境：列出 `display_environment` 中的 Windows mode Hz、DWM Hz、DWM period 和 `Present` sync interval。
4. 结果：列出 `win32 drag move interval p95`、`input -> axis limits p95`、`input -> present p95`、`present interval p95`、`implot pan sample interval p95`。
5. 诊断：如果失败，说明失败发生在输入到达、ImPlot 采样、render pass 还是 Present。
6. 结论：使用脚本末尾 `Result: PASS/FAIL`，只声明该日志证明的刷新率目标，不外推到真实数据或其他交互。

## 当前真实数据基线

截至 2026-06-21，`logs/specforge-profile-20260621-064415.jsonl` 来自真实 `.npy` 数据
`carbon_net_increment_loglam_V0.31_X.npy` 的主图 pan/drag 采集。该日志在 130Hz 预算
`BudgetMs 7.6923` 下通过，在 144Hz stretch 预算 `BudgetMs 6.9444` 下失败。

144Hz 失败项是 `win32 drag move interval p95=7.099 ms`、`implot pan sample interval p95=7.268 ms`
和 `present interval p95=7.115 ms`；`input -> present p95=6.940 ms` 和
`view_update duration p95=5.039 ms` 仍满足 144Hz 预算。因此当前可以声明该真实数据 pan/drag
场景通过 130Hz 验收，不能声明 144Hz stretch 已达标。

## 失败解释规则

- `win32 drag move interval` 慢：输入事件本身没有足够高频，先查系统、鼠标、窗口消息路径。
- `win32 drag move interval` 快，但 `implot pan sample interval` 慢：输入到了，但 ImGui/ImPlot 或主循环没有每帧采样。
- `implot pan sample interval` 快，但 `input -> present` 慢：交互更新到了，瓶颈更可能在 render pass、Present 或帧节奏。
- `view_update` 慢：UI/plot 逻辑成本高，优先看点数、overlay、状态更新和 ImPlot 调用。
- `render_pass` 或 `present` 慢：优先看 DX11 swap chain、vsync、窗口状态和 GPU/显示器路径。

## 注意

当前 shell 同时保留 synthetic fixture 和真实数据 loader。Synthetic fixture 可以验证 shell 和 ImPlot 交互链路，但不能证明真实光谱数据达标。真实性能结论必须用实际 `.npy`、CSV 或 FITS 数据源按同一流程重新生成日志。
