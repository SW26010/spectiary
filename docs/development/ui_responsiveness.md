# UI 响应速度实现约束

## 定位

Spectiary 的 UI 响应速度是产品目标，不是后期优化项。主图 pan、wheel zoom、上一条/下一条、range navigation、labeling shortcut 和 panel 操作都必须保持跟手；sample filtering、sorting、labeling、annotation、source session view 或本地状态功能不能把重计算、IO、全量序列构造带进这些热路径。

`docs/testing/performance_testing.md` 记录如何采集和判断 ImPlot pan/drag profile。本文件维护现行实现约束、设计规则和变更检查清单；退化原因、修复过程和当时的采样保存在[历史复盘](../evidence/performance/ui-responsiveness-history.md)。

## 热路径边界

热路径包括：

- 每帧 render path，尤其是 `ShellUi::RenderMainPlot()`。
- ImPlot pan/drag 和 wheel zoom 期间的 `view_update`、draw submission、render pass、present 链路。
- sample previous/next、sample-name locate、label auto-advance 和 labeling shortcut。
- 打标签后的当前样本刷新和自动跳转。
- 侧栏开启时每帧构造的 source、session、navigation、sample filter、sorting 和 labeling view。

这些路径上允许做的事情应该是：

- 读取当前 snapshot handle 或小型 view。
- 读取已经由 owner 缓存好的 sequence、sample filter、sorting 和 session 状态。
- 提交轻量 command，并只在必要时加载目标 sample snapshot。
- 渲染当前 frame 需要的 UI。

这些路径上不应该做的事情是：

- 重新扫描 source folder 或重建 source identity。
- 每帧构造全量 sample sequence rows、sample filter sources、sorting values 或 evaluation mask。
- 为了拿当前 snapshot 调用 full `SessionView()`。
- 在 session 状态未变化时重复构造同一个 session view，无论是否位于同一 frame。
- 在 label write 后重复同步 navigation/source workflow。
- 把只给面板展示的完整列表复制到 plot render path。

## 设计规则

1. Full view 不是 snapshot accessor。任何只需要当前光谱的 UI 必须使用 cheap snapshot API，不能调用 full session/workflow view。
2. 每帧 view 只能是 presentation snapshot，不能隐式做 source scanning、sample filter evaluation、sorting value extraction 或 sequence materialization。
3. Deep module 可以计算完整 domain result，但 UI-facing view 要按显示需求裁剪；完整 rows、masks、values 应留在 owner 或测试 seam。
4. Plot/overlay render path 只能读取其实际渲染所需的窄 view。Spectral-line overlay 使用 `PlotView()`；完整 `CatalogUserStateView` 只服务 panel 展示。
5. Catalog user state 的不变量在 load 和 persistent intent 后规范化；per-frame `View()` 只投影 owned state，不负责重复修复或补全 grouping views。
6. 业务 owner 负责缓存和失效。UI panel 不应该自己推断 sample filter/sequence/sorting，也不应该用拷贝出来的大列表判断权限。
7. Source identity 和 workflow context 要分开。sample index 切换不等于 source identity 变化，也不应该触发 folder scan 或 sample filter/sort reapply。
8. Sample filter/sorting/sequence 的默认 source-order 场景必须是 cheap path；只有 active sample filter/sort 或调试/test seam 需要 materialized ordered rows。
9. Label write 只能做一次必要同步。写值、保存、sample filter update、auto-advance 要有清楚顺序，避免 command result 后再做全量 ensure。
10. 新 panel 抽取或 session boundary refactor 必须检查每帧调用点。结构更干净不自动代表响应更快。
11. 单元测试只能证明规则正确，不能证明交互预算。触碰热路径时必须补 profile 或至少解释为什么该改动不进入热路径。
12. 长 build 不是合格反馈环。人工或 agent 验证 full CMake build 时必须带超时；性能问题优先建立可重复 profile 或 focused compile/test loop。
13. Folder navigation 可以复用上一次 post-decode 验证通过、且仍 current 的 immutable listing generation；notification 不可用时才保留 fresh post-decode full scan fallback。cache hint 不能升级为 source-of-truth。
14. Known-source context 只能在已提交 identity、source/companion/annotation dependency proof 和 folder generation（若适用）均通过两阶段检查时复用；单独的 identity 或 generation hint 不能跳过 manifest materialization。
15. Resident snapshot 由 roster 按 raw row 和完整 source/context/generation 边界管理；session 只选择候选，load worker 在后台完成 currentness/TOCTOU 验证后才能跳过 decode。命中、淘汰、取消和 stale completion 都不能绕过原 pending/commit/supersede 事务。
16. Adjacent prefetch 只能在前台导航成功激活后，从同一 sample-filtered/sorted sequence 按当前方向选择 raw row；同一时刻最多一个 below-normal worker，必须可被任何新前台意图取消，并使用不阻塞 foreground ordered publication 的独立完成通道。prefetch drain 只写 roster residency，不能激活、移动 index 或提交 workflow。
17. Panel-facing session mutation/read 协议由 Shell-owned interaction module
    统一持有。Panel 只提交用户 intent 并消费返回的新 projection；action merge、
    mutation 后重读和 latency classification 不能散落回 panel 或逐 panel lambda。

## 推荐实现形态

优先采用：

- `CurrentSampleSnapshot()` 这类 narrow read API。
- `SpectralLinesPanelController::PlotView()` 这类只投影当前 render 所需状态的 narrow view。
- invalidation-driven view cache；状态未变化时跨帧复用，command 和 maintenance 后显式 dirty。
- owner-owned cache，例如 navigation sequence cache、sample filter view cache、sorting view cache。
- roster-owned bounded resident snapshot LRU；淘汰项继续交给 background reclaimer，不在 UI thread 释放大 payload。
- 单任务、可取消、unordered publication 的 low-priority snapshot prefetch lane；foreground miss 永远不等待 speculative completion。
- 在 load/mutation 路径规范化 owned state，让 read view 保持纯投影。
- context fingerprint 驱动失效，而不是每次 sample index 变化都重算。
- source-order implicit representation，避免 `[0..N)` 常规场景分配和复制。
- session view 暴露 count、position、availability、current row 等 UI 需要字段，不暴露大列表作为常规合同。
- profile event 把 input、axis limits、view update、draw submission、render pass、present 分开，方便判断卡在 UI 逻辑还是帧节奏。

避免采用：

- 为了修卡顿绕过 domain owner，让 UI 直接读取 sample filter mask 或 labeling task 内部数组。
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
- sample navigation、sample filtering、sorting 与 labeling command path
- source folder / collection identity / snapshot loading
- plot render、overlay render、range controls

检查项：

- 主图 render path 是否仍只读取当前 snapshot 和 plot/overlay 所需轻量状态。
- Spectral-line plot 是否只读取 `PlotView()`，没有为 overlay 构造完整 `CatalogUserStateView`。
- Catalog user state 是否在 load/persistent intent 后规范化，而不是由 per-frame `View()` 重复修复。
- 每帧是否会构造 full sequence、sample filter source、sorting source、included samples 或 source identity。
- previous/next 和 label auto-advance 是否只加载目标 sample，不触发 source/workflow 全量重同步。
- panel view 是否只包含显示需要的状态，没有复制 owner 内部大列表。
- cache 失效条件是否覆盖 source、annotation、labeling、sample filter/sort choice、workflow context 和 maintenance 可见状态变化。
- panel mutation 是否全部经过 `PanelSessionInteraction`，且每个 panel 边界只消费一次聚合 action。
- resident snapshot 的 key 是否使用 raw row 和完整 source/context/generation 边界，cache hit 是否真的跳过 decoder，淘汰是否走后台 retirement。
- 新增测试是否覆盖规则正确性；新增或更新 profile 是否覆盖交互预算。
- 运行 build/test/profile 命令时是否设置了合理超时，避免诊断卡死。

## 历史复盘索引

- [2026-06-21：session view 退化](../evidence/performance/ui-responsiveness-history.md#复盘2026-06-21-session-view-退化)
- [2026-06-29：sample navigation sequence 退化](../evidence/performance/ui-responsiveness-history.md#复盘2026-06-29-sample-navigation-sequence-退化)
- [2026-07-17：catalog user state 投影退化](../evidence/performance/ui-responsiveness-history.md#复盘2026-07-17-catalog-user-state-投影退化)
- [2026-07-18：Portable Release 普通窗口退化](../evidence/performance/ui-responsiveness-history.md#复盘2026-07-18-portable-release-普通窗口退化)
- [2026-07-23：folder navigation 重复扫描及后续演进](../evidence/performance/ui-responsiveness-history.md#复盘2026-07-23-folder-navigation-重复扫描)
