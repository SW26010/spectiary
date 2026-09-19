# 上一条/下一条导航延迟测试

[性能测试入口](../performance_testing.md) · [测试导航](../README.md)

## 场景与输入口径

运行时录制会为显式标记来源、并且真正触发异步 source load 的上一条/下一条操作写
`navigation_latency` 事件。来源不是根据通用 `navigation.moved` 结果反推，而是由具体
command call site 传入：左右键分别为 `keyboard_previous` / `keyboard_next`，上一条/下一条
按钮分别为 `ui_previous` / `ui_next`，标签 workflow 自动推进为 `auto_advance`。`LocateRow`、
名称搜索跳转和其他通用导航命令不会混入这些统计。没有移动、没有目标或不需要异步加载
的命令也不会伪装成一次 spectrum switch。

键盘起点是应用消息循环从任意 Spectiary HWND（包括 detached viewport）取出初次
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

## 阶段指标与目标解析

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
  `sequence_build_count` 是本次实际构造次数。sequence state 只缓存排序拓扑、active membership
  和 source-row position；committed/pending/base/target index 只做 O(1) cursor 投影，名称搜索
  query 只基于既有 topology 重算 matches，两者都不使 topology 失效。sample filter、sort 或
  source/context 输入改变时才完整失效并由 owner 重建；label advance 的 eligibility 仍在每次
  `target_lookup_ms` 内按需计算，不进入缓存。
  因此 warm previous/next 应记录 `true / 0`；真正 cold 的首次 target resolution 最多构造一次，
  记录 `false / 1`。

## 缓存与预取的解释边界

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
（若适用）和 raw row index。query/sample filter/sort 只改变访问顺序，不复制 snapshot；source、
文件内容、annotation/context 或 directory generation 变化会 miss 并在新结果提交时清空旧
边界。前台 Previous/Next/auto-advance 成功激活后，会按同一 sample-filtered/sorted sequence 和当前
方向只选择下一条 raw row 做低优先级预取。预取使用一个可取消的 unordered publication
任务，不经过 `EnqueueBatch`，也不会阻塞之后的前台有序完成；任何新输入、反向、source/query/
sample filter/sort/context 变化都会取消或使旧结果失效。worker 仍执行 source/context/generation
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

## 日志兼容与字段校验

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

## 采集与配对比较

采集时先在 `Settings > Diagnostics` 开始录制，用真实数据连续执行若干次上一条/
下一条，等最后一条显示后再停止录制。然后运行：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\analyze-navigation-profile.ps1 logs\spectiary-profile-YYYYMMDD-HHMMSS-mmm.jsonl
```

比较 UI 与键盘 target resolution 时必须固定同一 source、同一 sample filter/sort/query 状态和同一
source-row 区间，分开录制纯 UI 与纯键盘各至少 100 次成功 presented navigation。每次输入要等
目标显示后再继续，除非实验明确要比较 rapid-navigation pending path；普通输入对比要求
`pending_present=false`。记录 source、起止 index、方向、样本数和 profile 路径，不能把不同
目录、不同 index window 或 Previous/Next 混成输入设备差异。

`spectiary_shell_source_load_activation_tests` 还在同一 synthetic source 的固定 `0 ↔ 1`
窗口，对 UI/keyboard 的 Previous/Next 各执行 100 次 presented 复测，并输出
`base_sequence_ns`/`target_sequence_ns` 的 p50/p95。它锁定 accepted-command 之后的 input kind、
cache/build 计数、load/activation/Present 行为和投影成本；它不模拟物理点击/按键，也不替代真实
Portable source 的 JSONL A/B。真实 IO/decode/context/renderer phase 是否无回退，仍以随后实际
数据采集为准。

预取 A/B 必须在同一 active source、同一 query/sample filter/sort/context 和同一 index window 下，
将连续 Next 与反向 Previous 分开各录制至少 100 次。最终并列记录 snapshot hit rate、
`total_ms` p95、`decode_ms` p95、`activation_ms` p95，以及 `queue_wait_ms`/
`completion_service_wait_ms` p95；同时确认
`sequence_cache_hit=true/false` 两组仍可解释、稳定 folder 为 `listing_scan_performed=false`
且 `context_reused=true`。这些数字只能来自对应的完整 JSONL，不能用 synthetic test 时间估算。

## 完整性门禁与结果解释

Portable build 的实际日志通常在 `logs\`，也可以把对应完整路径传给脚本。分析器会
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
