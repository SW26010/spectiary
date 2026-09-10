# UI 响应速度实现约束与复盘

## 定位

SpecForge 的 UI 响应速度是产品目标，不是后期优化项。主图 pan、wheel zoom、上一条/下一条、range navigation、labeling shortcut 和 panel 操作都必须保持跟手；sample filtering、sorting、labeling、annotation、source session view 或本地状态功能不能把重计算、IO、全量序列构造带进这些热路径。

`docs/performance_testing.md` 记录如何采集和判断 ImPlot pan/drag profile。本文件记录实现层面的约束、已发生的退化原因、修复方式和后续设计规则。

## 热路径边界

热路径包括：

- 每帧 render path，尤其是 `ShellUi::RenderMainPlot()`。
- ImPlot pan/drag 和 wheel zoom 期间的 `view_update`、draw submission、render pass、present 链路。
- sample previous/next、sample-name locate、label auto-advance 和 labeling shortcut。
- 打标签后的当前样本刷新和自动跳转。
- 侧栏开启时每帧构造的 source/session/navigation/filter/sorting/labeling view。

这些路径上允许做的事情应该是：

- 读取当前 snapshot handle 或小型 view。
- 读取已经由 owner 缓存好的 sequence/filter/sorting/session 状态。
- 提交轻量 command，并只在必要时加载目标 sample snapshot。
- 渲染当前 frame 需要的 UI。

这些路径上不应该做的事情是：

- 重新扫描 source folder 或重建 source identity。
- 每帧构造全量 sample sequence rows、filter sources、sorting values 或 evaluation mask。
- 为了拿当前 snapshot 调用 full `SessionView()`。
- 在 session 状态未变化时重复构造同一个 session view，无论是否位于同一 frame。
- 在 label write 后重复同步 navigation/source workflow。
- 把只给面板展示的完整列表复制到 plot render path。

## 复盘：2026-06-21 session view 退化

范围：`4b3d6ee699dca8834f7f90ff9c678207838981bc` 及之前没有明显卡顿；从 `7d820223f238eebfe1dc2b8bc2c4179ab96dbd0b` 开始，UI 响应变慢；`e767293b23a1591d8c4705a85451fcce0ecbacbf` 修复。

引入原因：

- `7d820223` 把 source collection 操作收敛到 session command/view 边界，这是方向正确的结构化改动。
- 但新的 `SourceCollectionSession::View()` 变成了宽 view 聚合：一次构造 source list、navigation、labeling、filter 等多个子视图。
- `ShellUi` 多个 panel 和主图代码直接调用 `session_.View()`；主图 pan 期间，为了拿 snapshot 也会付出 full session view 构造成本。
- filter view 构造复用了 domain evaluation 所需的全量 filter source builder，把本该只在状态变化时做的 work 变成每帧 work。

修复方式：

- 在 `ShellUi` 增加 `SessionView()` cache，同一帧多个 panel 共用一次 view，session command 后标脏；2026-07-18 起该 cache 在状态未变化时跨帧保留。
- 把 filter panel view builder 和 domain filter evaluation builder 拆开；面板展示只构造 id、name、option summary，不重建每个 sample 的 evaluation 输入。
- 用 profile 脚本和真实 `.npy` 数据重新验证主图 pan/drag，而不是只凭单元测试通过判断。
- `docs/performance_testing.md` 在该修复中补充了 130Hz acceptance、144Hz stretch、真实数据基线和报告口径。

判断：这个修复是必要且规范的。它没有绕开 session boundary，也没有引入自研渲染路径，而是在 owner 边界内缓存可复用 view，并把展示用数据和 evaluation 用数据拆开。主要不足是当时的保护仍偏向 pan profile，没有把“当前 snapshot 读取必须 cheap”写成 API 约束，后续 sample navigation sequence 工作又踩到了相近问题。

后续硬化：

- `SourceCollectionSessionResult` 只表达 command action、navigation result、changed/loaded/message，不携带 `SourceCollectionSessionView`，避免 command 提交默认构造 full session view。
- panel 交互统一经过 Shell-owned `PanelSessionInteraction`：提交 intent 后由该
  module 重读 invalidation-driven view、聚合 action，并把 previous/next 与 label
  auto-advance 映射到 latency classification；panel 不自行组合 submit/read 回调。
- plot、smoothing、information、spectral lines 等 snapshot-only surface 继续使用 `CurrentSampleSnapshot()`，不通过 full session view 取当前 sample。

## 复盘：2026-06-29 sample navigation sequence 退化

范围：`7c02e87d7cb6441be8159396b05a99887349fb8f` sample navigation sequence、filtering、sorting、empty sequence 等能力接入后，用户反馈上一条/下一条、打标签和主图 pan 都有卡顿感；随后通过缩窄热路径恢复正常。

引入原因：

- 新 sequence/filter/sorting 逻辑把正确的业务规则推进了 navigation-owned seam，但一开始把较重的构造放到了 `SourceCollectionSession::View()` 和 per-frame UI 路径中。
- `RenderMainPlot()` 等只需要当前 sample snapshot 的地方，通过 full session view 间接获取 snapshot，导致主图 pan 也触发 navigation/filter/sorting view 构造。
- `NavigationView()` 会构造 sequence；早期实现还会 materialize source-order rows 和 active `sequence_rows`，再复制进 per-frame view。
- sorting/filter panel 每帧重建 source views、sort values、filter evaluation 或 included sample lists。
- previous/next、label auto-advance 通过 `LoadActiveSourceAt()` 加载 snapshot 后又调用 workflow sync；folder source 下这会重复构造 source collection identity，等价于在切换样本时扫描/stat 整个 folder。
- label write 后还有冗余的 snapshot/navigation ensure，同一用户动作里重复做同步工作。

修复方式：

- 增加 cheap snapshot accessor，只返回当前 sample snapshot，不构造 full session view。主图、smoothing、information、spectral lines 等 snapshot-only call site 改用该 accessor。
- `SampleNavigationController` 持有 sequence topology cache；navigation read path 通过 cached
  topology 解析 previous/next、label auto-advance 和 row location policy。sample-name matches
  只在用户实际输入、恢复或提交 query 时按 cached topology 重新扫描。committed/pending cursor
  和 query 都不因 current index 或搜索文本变化重建 topology。
- `SampleNavigationSequence` 支持不 materialize source-order rows；active membership 用 mask
  表示，排序后的 source row → sequence position 也随 topology 缓存，`ContainsSourceRow()` 和
  cursor projection 都走 O(1)。
- per-frame `SourceCollectionNavigationView` 不再复制 `sequence_rows`，只暴露 sequence-facing 状态，例如 active flag、sequence count、current sequence position、source row index 和 row location availability。
- filter view 和 sorting source view 建 cache，只在 source、annotation、labeling、filter/sort choice 或 workflow context 变化时失效。
- `SyncActiveSource()` 只在 source identity 或 workflow context fingerprint 变化时 reapply filter/sorting；纯 current-index snapshot load 不再重建工作流状态。
- label assign/clear 去掉写入后的冗余 ensure。
- 验证时优先用 focused tests、direct compile probes 和 `git diff --check`；避免无超时的 full CMake build 卡住反馈环。

判断：这个方案比单纯“少调用几次函数”更标准。业务规则仍然归 navigation/filter/sorting owner 管，UI 不偷读内部状态；性能约束通过 API 形状表达为 cheap snapshot accessor、cached sequence、cached filter/sort views 和不 materialize rows 的默认路径。它的取舍是 session view 不再携带完整 row 列表，测试需要改为验证 count/position/target 行为；这是合理取舍，因为完整列表属于 deep model/debug 数据，不该成为每帧 UI 合同。

## 复盘：2026-07-17 catalog user state 投影退化

范围：`fee63a6892cc64866ffc5eae0fa3ec18615b956a` 深化 spectral-line catalog user state Module 后，用户反馈 UI 性能明显退化；`b3be6d441c22d6092574e2fe2ca3f2ef1bfb1901` 修复。

引入原因：

- `fee63a6` 把 catalog user state 收敛为 intent/result/view Interface，并由 Module 统一验证 identity、不变量、dirty 状态和持久化；这个结构方向正确。
- 但普通与沉浸式 plot 为了读取 marker label visibility，在每帧调用完整 `SpectralLinesPanelController::View()`；该 view 同时投影全部 grouping views、groups、marker references、grouping view search 和 panel 状态。
- `View()` 还会对每个 user grouping view 重复调用 `EffectiveUserGroupingView()`。owned state 已经在 load 和 persistent intent 后规范化，这次 per-frame 修复既冗余，又让 plot `view_update` 成本随 grouping view set 增长。
- 结果是只需要当前可见 spectral-line markers 和一个 label flag 的 overlay render path，承担了只属于 panel 展示的完整 catalog user state 投影。

修复方式：

- 增加窄 `SpectralLinesPanelController::PlotView()`，只返回当前 plot-visible spectral-line markers 和 marker label visibility；普通与沉浸式 plot 不再读取完整 `CatalogUserStateView`。
- `View()` 直接投影 Module 持有的规范化 user grouping views，不在每帧 read path 重复执行 `EffectiveUserGroupingView()`。规范化继续由 constructor load 和每个 persistent intent 的统一完成路径负责。
- controller Interface 测试覆盖 missing/unsupported snapshot、catalog marker 顺序、marker visibility、marker label visibility，以及既有的修改、选择恢复和持久化行为。
- 使用 `carbon_net_increment_loglam_V0.31_X.npy` 的 3202 条 spectrum，在本机 Windows/DWM 约 120Hz 环境完成 16.17 秒真实 pan/drag：present interval p95 为 9.310ms、input-to-present p95 为 9.081ms、`view_update` p95 为 0.681ms。相对上一 commit 的 present interval p95 9.230ms，差异约 0.08ms，按本次“无明显相对退化”口径通过。

判断：深化 catalog user state Module 本身没有错，问题是把 panel-facing full view 当成 plot overlay accessor。修复把性能特征写进 Interface 形状，在不绕过 Module、不复制 catalog marker 数据、不引入缓存失效协议的前提下恢复了 Locality。上述 10ms 是本次回归诊断门槛，不替代 `docs/performance_testing.md` 定义的 130Hz 项目验收标准；本次采集不能单独声明达到 130Hz acceptance。

## 复盘：2026-07-18 Portable Release 普通窗口退化

现象：同一台内屏 DRR 120Hz 环境中，Portable Release 的普通窗口 pan 明显不流畅，沉浸模式正常。呈现诊断仍显示 composition backend、无降级、requested/actual 120Hz，因此不是 DRR 或合成时钟失效。

引入原因：

- `ShellUi::Render()` 在每个普通 frame 的开始和结束都主动丢弃 `session_view_cache_`；原有 cache 实际只有 frame-scoped 生命周期。
- 普通模式的 Files 等 panel 会读取完整 `SourceCollectionSessionView`，沉浸模式只走窄 plot view，因此仅普通窗口命中该成本。
- 在 11,550 条 folder collection 及已恢复的 navigation/labeling 状态下，完整 `SessionView()` 重建 p50/p95 为 22.736/26.994ms，并且每帧恰好重建一次。修复前 `view_update` p50/p95 为 26.967/31.258ms，present interval p50/p95 为 27.704/32.125ms。

修复方式：

- `SourceCollectionSession` 统一拥有 projection revision、跨帧 cache 和旧 generation
  retirement；Shell/panel 只读取稳定 projection、提交 intent。
- 只有 roster、snapshot、workflow、navigation 或 labeling presentation revision
  确认变化时才提升 revision；no-op intent 和无变化 maintenance 不重建。
- 旧 projection 保留到当前 UI frame 结束，再交给后台 reclaimer，避免 panel
  持有借用引用时并发析构。
- 不绕过 `SourceCollectionSession`，不把 filter/sorting/labeling 内部状态泄漏给 UI，也不改变沉浸模式或 presentation contract。

验证：同一份 11,550 条数据、同一份 Portable 本地状态和同一普通窗口 mouse-pan 流程中，1,464 个普通帧只有启动/维护边沿的 2 帧重建 view，其余 1,462 帧重建次数为 0。`view_update` p50/p95 降为 4.155/5.335ms，present interval p50/p95 降为 8.339/9.368ms；用户主观确认恢复流畅。严格 8.3333ms p95 gate 仍受 120Hz 帧节奏尾部影响，本次结论是消除普通窗口相对沉浸模式的 CPU 退化，不是宣称完整 120Hz gate 已通过。

剩余边界：发生可见 session mutation 或 maintenance 状态变化后，完整 view
仍会一次性重建；若 future profile 显示 previous/next、labeling 或 filter/sort
command 的单次延迟受此影响，应继续缩窄或 owner-cache 对应子 view，而不是恢复
per-frame 重建。当前 headless session regression 会验证稳定读取、no-op command、
load completion、maintenance、roster mutation 与 pending cancellation 的精确失效；
上述 Release A/B profile 继续锁定真实数据下的 steady-state 性能基线。

## 复盘：2026-07-23 folder navigation 重复扫描

现象：完整且 `dropped_events=0` 的
`specforge-profile-20260723-110532-476.jsonl` 包含 385 次 folder load attempt。
folder attempt 的 `source_inspection_ms` p50/p95 为 31.964/133.228ms，
`source_revalidation_ms` p50/p95 为 18.106/137.465ms，而 decode p50/p95
仅为 7.607/15.241ms。一次代表性翻页在解码前扫描 127.536ms、解码后扫描
133.496ms，最终 input-to-Present 为 331.560ms。

引入原因：

- `SourceCollectionLoadQueue::PrepareFolder()` 每次翻页先扫描并排序整个目录，用同一
  listing 解码 snapshot 和构造 source identity/context。
- 为保证 snapshot、identity、annotation dependencies 属于同一稳定 filesystem
  generation，解码完成后又扫描整个目录并逐项比较；一致性保护正确，但稳定目录的每次
  翻页都支付两次完整枚举成本。
- 已成功加载的 source session 只保留 snapshot 和 workflow identity，没有保留完成
  post-decode revalidation 的 immutable folder listing，因此下一行无法复用已有观察。

修复方式：

- worker 成功后把 post-decode revalidation 得到的 listing 作为 immutable shared
  handle 随 prepared result 发布，由 source roster 按 source entry 保留；下一次 load
  request 只把它作为 optimization hint 传回 worker，不把 UI 变成 filesystem owner。
- warm navigation 先用 O(1) 的目标文件 stat 检查保护 stale/deleted target，然后复用
  listing 解码；解码后仍执行一次完整扫描。只有 fresh post-decode listing 与候选
  generation 完全匹配时才发布 snapshot。
- 如果目录在加载期间变化，fresh listing 会成为下一轮候选并重新解码，随后再次完整
  revalidate。缓存不会绕过 TOCTOU protection，也不会让 stale identity 进入 session。
- folder scanner 进入 loader dependency seam；测试明确锁定稳定 warm navigation 为一次
  完整扫描、stale target 为 refresh + revalidation 两次扫描、并发目录变化为每个 decoded
  generation 一次扫描。

真实复测：完整且 `dropped_events=0` 的
`specforge-profile-20260723-115613-134.jsonl` 包含 371 次 folder load attempt。
`source_inspection_ms` p50/p95 已降为 0.266/1.082ms，证明 warm path 不再执行完整
pre-decode 枚举；`source_revalidation_ms` p50/p95 为 18.103/137.226ms，证明一次完整
post-decode scan 仍在。437 次 presented navigation 的总体 input-to-Present p95 为
198.130ms，尾部仍主要来自 revalidation，而不是被误移到 inspection。

复测也暴露了一个较小但真实的 UI-thread ownership 尾部：以 revalidation 80ms 为界，
小/大目录的 activation p95/max 分别为 5.42/7.81ms 和 7.28/9.22ms；revalidation
成本与 activation 的相关系数约为 0.73。代码检查确认 roster 替换 listing 时，旧
listing 的最后一个引用可能在 UI thread 同步析构。修复沿用现有 background reclaimer：
roster 把被替换的 listing 随 prepared-open result 交回 session，session 将原
listing generation handle 直接作为 opaque retirement token
返回 Shell；不新增线程、队列或 source-of-truth。回归测试锁定旧 listing 在
`background_retirement` drain 前保持存活，现有 reclaimer test 继续保证实际析构不在
调用线程。这里的 listing reuse 不是 adjacent spectrum snapshot cache，
`navigation_latency.cache_hit` 仍保持原语义。

随后完整且 `dropped_events=0` 的
`specforge-profile-20260723-121328-397.jsonl` 再次确认剩余热点。312 次 presented
navigation 中，keyboard Next 的 `source_revalidation_ms` p50/p95 为
16.825/146.121ms，UI Next 为 142.839/146.576ms；相对地 keyboard Next 的
`source_inspection_ms` p50/p95 仅为 0.235/0.323ms，decode 为 0.791/5.463ms。
这证明 warm pre-decode 路径已足够窄，而每次 post-decode 全目录枚举、排序和逐项比较
仍主导长尾。

第二阶段修复把 immutable listing 与一次性 directory-change generation 绑定：

注册线程的生命周期、无轮询等待和取消边界见
[ADR 0008](adr/0008-directory-change-registration-lifetime-and-wait.md)。
加载依赖的逐项保留理由及生产接口测试边界见
[ADR 0009](adr/0009-source-loading-dependency-boundaries.md)。

- worker 在完整扫描前先建立只观察目录第一层的 generation boundary；同一 generation
  内的 listing 由 source roster 保留并随下一次请求作为 opaque cache hint 返回。
- Windows generation 由 change notification 提供，文件/目录名、attributes、size、
  last-write、creation 或 security 变化都会让该 generation 永久失效；测试覆盖新增、
  成员修改、重命名和删除。
- warm navigation 仍先对目标 spectrum 做 O(1) stat，解码和 context 构造后再检查
  annotation/source dependency state 与 generation。generation 未变时不再重新枚举、
  排序或比较整个目录；失效时重扫并按同一最多两轮的 TOCTOU 合同重新解码。
- change notification 不可用时，不信任 cache，保留原有 post-decode full
  scan-and-compare 作为 correctness fallback。
- listing generation 被替换、source 被移除或 prepared result 被拒绝时，继续交给现有
  background reclaimer 释放，避免大 listing 或 change handle 在 UI thread 析构。

自动化回归锁定 stable warm generation 为 0 次 full scan、stale target 为 1 次 refresh
scan、加载期间目录变化只扫描 replacement generation，以及 notification 不可用时仍执行
1 次 full revalidation。真实 Release profile 仍需重新采集，才能量化
`source_revalidation_ms` 与 input-to-Present 的新分布；这些测试只证明调用次数和失效合同。

第三阶段的修改前 Release 基线是
`dist/SpecForge-portable/Data/logs/specforge-profile-20260723-155759-657.jsonl`。
在重建会清空 package `Data` 的 Portable 包之前，已将同 SHA-256 的本地证据副本保存在
`logs/specforge-profile-20260723-155759-657.jsonl`。
该日志 summary 完整、`dropped_events=0`，582 次导航中 578 次 presented、4 次正常
superseded。navigation analyzer 的 `Result: PASS` 只证明日志结构、时间戳关联和录制完整，
不表示达到某个导航预算；通用 pan analyzer 因只有一次 57.3ms 的偶发拖拽、没有合格测试
窗口而产生的 FAIL 与本次导航分析无关。

按 source 类型和连续导航区间拆分，修改前结果为：

| 区段 | 样本数 | total p95 | 约合 120Hz 帧数 | 主要耗时 |
|---|---:|---:|---:|---|
| file | 164 | 21.60ms | 2.6 帧 | context、activation、UI |
| folder A | 121 | 25.13ms | 3.0 帧 | decode 11.18ms |
| folder B，最慢 | 178 | 51.94ms | 6.2 帧 | context 24.59ms |
| folder C，小型 | 34 | 15.16ms | 1.8 帧 | decode、activation |
| folder D | 81 | 25.05ms | 3.0 帧 | decode/context 各约 8ms |

最慢 folder B 的 p95 中，`context_prepare=24.59ms`、`decode=7.44ms`、
`target_resolution=6.47ms`、`ui_update=7.37ms`、`activation=6.07ms`；
inspection、revalidation、queue wait 和 UI-to-Present 均已很小。414 个 folder round
全部为 `hint_present=true`、`generation_current=true`、`listing_scan=false`，因此该
24.59ms 不是目录扫描，而是 stable warm navigation 仍在复用判断之前重建完整 identity、
sample-name manifest 和 annotation manifest。仅扣除此项的估算残余 p95 为 28.70ms；
这只是定位优先级，不替代修改后的实测。

第三阶段修复把上一次成功提交时的 `SourceCollectionIdentity` 与 source/companion/
annotation dependency state 组成 context reuse proof，并由 roster 与同一次接受的 snapshot
和 folder generation 一起持有。worker 仍在 decode 前后各捕获一次 dependency state；
folder 还要求原 immutable listing generation 从开始到 post-decode 都 current。只有 proof、
known identity、decoded count/path 和两次 dependency state 全部一致时，才直接发布
`PreparedSourceCollectionReuse`，不调用完整 context builder。source、companion、
annotation、目标 member 或 generation 任一变化，或 change generation 不可用时，均回退到
原完整 materialize/TOCTOU 路径。取消、retarget、有序发布和 background retirement 的所有权
不变。

自动化测试直接统计 full file/folder context builder 调用次数：stable known-source warm
navigation 为 0；source、companion、annotation 或 generation 变化为 1；decode 期间变化时
第一轮 proof 被拒绝，仅稳定重试 materialize 一次。profile 的 attempt 和 preparation round
新增 `context_reused`，旧日志仍可由 analyzer 读取。

修改后的同数据 Portable Release 记录为
`logs/context-reuse-ab-after/specforge-profile-20260723-163115-308.jsonl`。日志 summary
完整、`dropped_events=0`，429 次导航中 421 次 presented、8 次正常 superseded；
navigation analyzer 为 `PASS`。全部 421 个最终 attempt 都是 `context_reused=true`：
file 110 个，`context_prepare` p50/p95 为 0.033/0.055ms；folder 311 个，
0.007/0.009ms。全部 311 个 folder round 仍为 `hint_present=true`、
`generation_current=true`、`listing_scan=false`。

同源 A/B 使用连续区段配对：修改后区段的起始 index 必须等于修改前该 source 的终止
index（file 580，folder A/B/C/D 分别为 494/6074/108/305）。另外三个没有 before
连续边界的新增 folder 区段不纳入下表。p95 使用 nearest-rank，与修改前分析口径相同：

| 区段 | before/after 样本数 | total p95 before → after | context p95 before → after | after 约合 120Hz 帧数 |
|---|---:|---:|---:|---:|
| file | 164 / 110 | 21.60 → 13.76ms | 4.18 → 0.06ms | 1.7 帧 |
| folder A | 121 / 42 | 25.13 → 18.71ms | 4.06 → 0.01ms | 2.2 帧 |
| folder B，修改前最慢 | 178 / 74 | 51.94 → 31.59ms | 24.59 → 0.01ms | 3.8 帧 |
| folder C，小型 | 34 / 23 | 15.16 → 15.88ms | 0.09 → 0.01ms | 1.9 帧 |
| folder D | 81 / 20 | 25.05 → 14.37ms | 7.54 → 0.01ms | 1.7 帧 |

这里最直接的验收证据是每个 stable warm attempt 的 `context_reused=true` 以及
`context_prepare` 降至测量噪声量级；不同窗口的样本数、方向、目标 spectrum 和其他阶段
并非逐项配对，因此 total p95 只用于同一真实数据环境下的整体 Release 结果，不据此把
folder C 的小幅波动归因于本修改。修改前最慢 folder B 的 context 浪费已消除，total p95
下降 20.35ms（39.2%），与修改前扣除 context 后 28.70ms 的估算残余处于同一量级；其
after `target_resolution` p95 为 7.12ms，属于明确不包含的下一个独立 owner。

## 设计规则

1. Full view 不是 snapshot accessor。任何只需要当前光谱的 UI 必须使用 cheap snapshot API，不能调用 full session/workflow view。
2. 每帧 view 只能是 presentation snapshot，不能隐式做 source scanning、filter evaluation、sorting value extraction 或 sequence materialization。
3. Deep module 可以计算完整 domain result，但 UI-facing view 要按显示需求裁剪；完整 rows、masks、values 应留在 owner 或测试 seam。
4. Plot/overlay render path 只能读取其实际渲染所需的窄 view。Spectral-line overlay 使用 `PlotView()`；完整 `CatalogUserStateView` 只服务 panel 展示。
5. Catalog user state 的不变量在 load 和 persistent intent 后规范化；per-frame `View()` 只投影 owned state，不负责重复修复或补全 grouping views。
6. 业务 owner 负责缓存和失效。UI panel 不应该自己推断 filter/sequence/sorting，也不应该用拷贝出来的大列表判断权限。
7. Source identity 和 workflow context 要分开。sample index 切换不等于 source identity 变化，也不应该触发 folder scan 或 filter/sort reapply。
8. Filter/sorting/sequence 的默认 source-order 场景必须是 cheap path；只有 active filter/sort 或调试/test seam 需要 materialized ordered rows。
9. Label write 只能做一次必要同步。写值、保存、filter update、auto-advance 要有清楚顺序，避免 command result 后再做全量 ensure。
10. 新 panel 抽取或 session boundary refactor 必须检查每帧调用点。结构更干净不自动代表响应更快。
11. 单元测试只能证明规则正确，不能证明交互预算。触碰热路径时必须补 profile 或至少解释为什么该改动不进入热路径。
12. 长 build 不是合格反馈环。人工或 agent 验证 full CMake build 时必须带超时；性能问题优先建立可重复 profile 或 focused compile/test loop。
13. Folder navigation 可以复用上一次 post-decode 验证通过、且仍 current 的 immutable listing generation；notification 不可用时才保留 fresh post-decode full scan fallback。cache hint 不能升级为 source-of-truth。
14. Known-source context 只能在已提交 identity、source/companion/annotation dependency proof 和 folder generation（若适用）均通过两阶段检查时复用；单独的 identity 或 generation hint 不能跳过 manifest materialization。
15. Resident snapshot 由 roster 按 raw row 和完整 source/context/generation 边界管理；session 只选择候选，load worker 在后台完成 currentness/TOCTOU 验证后才能跳过 decode。命中、淘汰、取消和 stale completion 都不能绕过原 pending/commit/supersede 事务。
16. Adjacent prefetch 只能在前台导航成功激活后，从同一 filtered/sorted sequence 按当前方向选择 raw row；同一时刻最多一个 below-normal worker，必须可被任何新前台意图取消，并使用不阻塞 foreground ordered publication 的独立完成通道。prefetch drain 只写 roster residency，不能激活、移动 index 或提交 workflow。
17. Panel-facing session mutation/read 协议由 Shell-owned interaction module
    统一持有。Panel 只提交用户 intent 并消费返回的新 projection；action merge、
    mutation 后重读和 latency classification 不能散落回 panel 或逐 panel lambda。

## 推荐实现形态

优先采用：

- `CurrentSampleSnapshot()` 这类 narrow read API。
- `SpectralLinesPanelController::PlotView()` 这类只投影当前 render 所需状态的 narrow view。
- invalidation-driven view cache；状态未变化时跨帧复用，command 和 maintenance 后显式 dirty。
- owner-owned cache，例如 navigation sequence cache、filter view cache、sorting view cache。
- roster-owned bounded resident snapshot LRU；淘汰项继续交给 background reclaimer，不在 UI thread 释放大 payload。
- 单任务、可取消、unordered publication 的 low-priority snapshot prefetch lane；foreground miss 永远不等待 speculative completion。
- 在 load/mutation 路径规范化 owned state，让 read view 保持纯投影。
- context fingerprint 驱动失效，而不是每次 sample index 变化都重算。
- source-order implicit representation，避免 `[0..N)` 常规场景分配和复制。
- session view 暴露 count、position、availability、current row 等 UI 需要字段，不暴露大列表作为常规合同。
- profile event 把 input、axis limits、view update、draw submission、render pass、present 分开，方便判断卡在 UI 逻辑还是帧节奏。

避免采用：

- 为了修卡顿绕过 domain owner，让 UI 直接读取 filter mask 或 labeling task 内部数组。
- 为了方便测试把完整 sequence rows 放进每帧 view。
- 为了读取一个 plot/overlay flag，在 render path 构造完整 panel-facing view。
- 在 per-frame view builder 中重复修复 owner 已经保证的不变量。
- 只延长 `SessionView()` cache 生命周期，却遗漏 command、maintenance 或其他 owner mutation 的失效边界。
- 未经 profile 就引入自定义 renderer、GPU path 或复杂 async pipeline。

## 变更检查清单

涉及以下区域时，提交前必须过一遍本清单：

- `ShellUi::Render*`
- `SourceCollectionSession::View()`
- `SampleWorkflowCoordinator::*View()`
- `SpectralLinesPanelController::View()` / `PlotView()`
- sample navigation/filtering/sorting/labeling command path
- source folder / collection identity / snapshot loading
- plot render、overlay render、range controls

检查项：

- 主图 render path 是否仍只读取当前 snapshot 和 plot/overlay 所需轻量状态。
- Spectral-line plot 是否只读取 `PlotView()`，没有为 overlay 构造完整 `CatalogUserStateView`。
- Catalog user state 是否在 load/persistent intent 后规范化，而不是由 per-frame `View()` 重复修复。
- 每帧是否会构造 full sequence、filter source、sorting source、included samples 或 source identity。
- previous/next 和 label auto-advance 是否只加载目标 sample，不触发 source/workflow 全量重同步。
- panel view 是否只包含显示需要的状态，没有复制 owner 内部大列表。
- cache 失效条件是否覆盖 source、annotation、labeling、filter/sort choice、workflow context 和 maintenance 可见状态变化。
- panel mutation 是否全部经过 `PanelSessionInteraction`，且每个 panel 边界只消费一次聚合 action。
- resident snapshot 的 key 是否使用 raw row 和完整 source/context/generation 边界，cache hit 是否真的跳过 decoder，淘汰是否走后台 retirement。
- 新增测试是否覆盖规则正确性；新增或更新 profile 是否覆盖交互预算。
- 运行 build/test/profile 命令时是否设置了合理超时，避免诊断卡死。
