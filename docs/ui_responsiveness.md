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
- 在一个 frame 内多次构造同一个 session view。
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

- 在 `ShellUi` 增加 frame-scoped `SessionView()` cache，同一帧多个 panel 共用一次 view，session command 后标脏。
- 把 filter panel view builder 和 domain filter evaluation builder 拆开；面板展示只构造 id、name、option summary，不重建每个 sample 的 evaluation 输入。
- 用 profile 脚本和真实 `.npy` 数据重新验证主图 pan/drag，而不是只凭单元测试通过判断。
- `docs/performance_testing.md` 在该修复中补充了 130Hz acceptance、144Hz stretch、真实数据基线和报告口径。

判断：这个修复是必要且规范的。它没有绕开 session boundary，也没有引入自研渲染路径，而是在 owner 边界内缓存可复用 view，并把展示用数据和 evaluation 用数据拆开。主要不足是当时的保护仍偏向 pan profile，没有把“当前 snapshot 读取必须 cheap”写成 API 约束，后续 sample navigation sequence 工作又踩到了相近问题。

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
- `SampleNavigationController` 持有 sequence cache；navigation read path 通过同一个 cached sequence 解析 previous/next、label auto-advance、sample-name match 和 row location policy。
- `SampleNavigationSequence` 支持不 materialize source-order rows；active membership 用 mask 表示，`ContainsSourceRow()` 走 O(1)。
- per-frame `SourceCollectionNavigationView` 不再复制 `sequence_rows`，只暴露 sequence-facing 状态，例如 active flag、sequence count、current sequence position、source row index 和 row location availability。
- filter view 和 sorting source view 建 cache，只在 source、annotation、labeling、filter/sort choice 或 workflow context 变化时失效。
- `SyncActiveSource()` 只在 source identity 或 workflow context fingerprint 变化时 reapply filter/sorting；纯 current-index snapshot load 不再重建工作流状态。
- label assign/clear 去掉写入后的冗余 ensure。
- 验证时优先用 focused tests、direct compile probes 和 `git diff --check`；避免无超时的 full CMake build 卡住反馈环。

判断：这个方案比单纯“少调用几次函数”更标准。业务规则仍然归 navigation/filter/sorting owner 管，UI 不偷读内部状态；性能约束通过 API 形状表达为 cheap snapshot accessor、cached sequence、cached filter/sort views 和不 materialize rows 的默认路径。它的取舍是 session view 不再携带完整 row 列表，测试需要改为验证 count/position/target 行为；这是合理取舍，因为完整列表属于 deep model/debug 数据，不该成为每帧 UI 合同。

## 设计规则

1. Full view 不是 snapshot accessor。任何只需要当前光谱的 UI 必须使用 cheap snapshot API，不能调用 full session/workflow view。
2. 每帧 view 只能是 presentation snapshot，不能隐式做 source scanning、filter evaluation、sorting value extraction 或 sequence materialization。
3. Deep module 可以计算完整 domain result，但 UI-facing view 要按显示需求裁剪；完整 rows、masks、values 应留在 owner 或测试 seam。
4. 业务 owner 负责缓存和失效。UI panel 不应该自己推断 filter/sequence/sorting，也不应该用拷贝出来的大列表判断权限。
5. Source identity 和 workflow context 要分开。sample index 切换不等于 source identity 变化，也不应该触发 folder scan 或 filter/sort reapply。
6. Filter/sorting/sequence 的默认 source-order 场景必须是 cheap path；只有 active filter/sort 或调试/test seam 需要 materialized ordered rows。
7. Label write 只能做一次必要同步。写值、保存、filter update、auto-advance 要有清楚顺序，避免 command result 后再做全量 ensure。
8. 新 panel 抽取或 session boundary refactor 必须检查每帧调用点。结构更干净不自动代表响应更快。
9. 单元测试只能证明规则正确，不能证明交互预算。触碰热路径时必须补 profile 或至少解释为什么该改动不进入热路径。
10. 长 build 不是合格反馈环。人工或 agent 验证 full CMake build 时必须带超时；性能问题优先建立可重复 profile 或 focused compile/test loop。

## 推荐实现形态

优先采用：

- `CurrentSampleSnapshot()` 这类 narrow read API。
- frame-scoped view cache，command 后显式 dirty。
- owner-owned cache，例如 navigation sequence cache、filter view cache、sorting view cache。
- context fingerprint 驱动失效，而不是每次 sample index 变化都重算。
- source-order implicit representation，避免 `[0..N)` 常规场景分配和复制。
- session view 暴露 count、position、availability、current row 等 UI 需要字段，不暴露大列表作为常规合同。
- profile event 把 input、axis limits、view update、draw submission、render pass、present 分开，方便判断卡在 UI 逻辑还是帧节奏。

避免采用：

- 为了修卡顿绕过 domain owner，让 UI 直接读取 filter mask 或 labeling task 内部数组。
- 为了方便测试把完整 sequence rows 放进每帧 view。
- 仅靠 memoizing `SessionView()` 掩盖内部仍然每帧重建 filter/sort/evaluation 的问题。
- 未经 profile 就引入自定义 renderer、GPU path 或复杂 async pipeline。

## 变更检查清单

涉及以下区域时，提交前必须过一遍本清单：

- `ShellUi::Render*`
- `SourceCollectionSession::View()`
- `SampleWorkflowCoordinator::*View()`
- sample navigation/filtering/sorting/labeling command path
- source folder / collection identity / snapshot loading
- plot render、overlay render、range controls

检查项：

- 主图 render path 是否仍只读取当前 snapshot 和 plot/overlay 所需轻量状态。
- 每帧是否会构造 full sequence、filter source、sorting source、included samples 或 source identity。
- previous/next 和 label auto-advance 是否只加载目标 sample，不触发 source/workflow 全量重同步。
- panel view 是否只包含显示需要的状态，没有复制 owner 内部大列表。
- cache 失效条件是否覆盖 source、annotation、labeling、filter/sort choice 和 workflow context 变化。
- 新增测试是否覆盖规则正确性；新增或更新 profile 是否覆盖交互预算。
- 运行 build/test/profile 命令时是否设置了合理超时，避免诊断卡死。
