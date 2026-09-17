# 2026-09-17：独立面板生产启用

状态：本地正式构建已默认启用，相关自动化回归通过；改动尚未推送或发布，未关闭 #56。

## 审查与决定

依据普通样式 A 的性能与体验正向证据、24 轮生命周期回归、人工窗口行为检查和有限完整
应用资源观察，将已有增量 buffer 算法提升为独立 ImGui viewport 的默认策略。未新增
资源算法、线程、平台 hook 或后端补丁。支持条件与预算回退见
[呈现策略](../../../presentation_policy.md#独立面板缩放资源策略)。

- 默认值及 Shutdown 后重置值均在独立 viewport renderer 中设置；低层 adapter 的默认值
  保持关闭，因此主窗口不受影响。
- 保留三个槽位、一个临时替换、每帧一次替换、可用事件非阻塞查询、最后提交槽位排除、
  完整构建后交换及已有 DXGI 回退。四阶段分配故障测试继续覆盖旧 generation 保留。
- 无收益的重定向/反馈频率实验仍由隔离诊断构建控制。其 backend 副本为 EXCLUDE_FROM_ALL
  的诊断目标，普通程序不读取相关环境变量。保留以重现历史对照，不赋予产品支持承诺。
- 诊断程序仍显式选择 baseline/incremental，正式程序不提供运行时实验配置。
- 保留既有事件与 legacy `buffer_replacement_experiment` 名称，避免破坏历史分析工具；
  enabled 表示策略设置，具体后端须读取 viewport transition。

## 验证

使用仓库 MSVC wrapper 配置并构建 `specforge_native`、`specforge_redirection_experiment`、
`specforge_sdr_swap_chain_tests`、`specforge_imgui_layout_persistence_tests` 和
`specforge_presentation_trace_tests`，均成功。

6 项针对性 CTest 最终均通过：swap-chain、layout persistence、presentation trace、viewport
policy architecture、profile schema、resource observation。首次 swap-chain 测试发现旧夹具
在同一 ImGui frame 内模拟多个应用帧，触发预期的每帧限额；夹具改为通过公开 NewFrame /
EndFrame 推进帧，修正后单独复跑通过（6.17 秒），其余五项先前已通过。

正式 renderer 生命周期测试现在直接验证普通样式 HWND 下的默认增量行为：只有初始完整
buffer rebuild，两个实际帧共一至两次单槽替换，尺寸与预算正确、事件查询不阻塞、Present
完成事件与实际回调一致。Shutdown/reinitialize 后策略仍启用。已有真实 Composition
分配失败回退和双窗口 24 轮回归通过；本机未跳过增量 Composition 测试。

本次不重复人工性能录制，不把自动化通过解释为新构建的主观流畅度测量。历史性能数据
仍只适用于原记录的构建与本机条件。剩余系统等待、跨硬件差异、预算触发的 DXGI 降级
以及长期资源稳定性限制仍适用。主窗口原生边框刷新继续单列 #101。
