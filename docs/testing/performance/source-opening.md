# 显式添加/打开数据源延迟测试

[性能测试入口](../performance_testing.md) · [测试导航](../README.md)

## 场景与时间边界

录制开启时，File 菜单或 Files 面板的文件/文件夹选择结果一经 `OpenSource` 接受，就会创建独立的
`source_load_latency` trace。命令行启动参数发生在运行时 recorder 状态进入 Shell UI 之前，不属于这条
“用户接受选择结果”的口径。trace 复用 source load queue 的阶段打点，但不会伪装成 Previous/Next
navigation。

`outcome=presented` 的终点与导航 trace 相同：最终目标 snapshot 已激活、UI 已更新，并且该精确 snapshot
已向 Spectrum viewport 提交 draw，随后该 viewport 完成第一次成功 `Present`。只完成 decode 或
`result.loaded` 不会提前结束 trace；失败、拒绝和被更新意图替代分别记录为 `failed`、
`rejected`、`superseded`。

## 事件与阶段指标

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

## 分析与完整性门禁

定量回归必须使用严格 analyzer，而不是只读取汇总字段：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass `
  -File scripts\analyze-source-load-profile.ps1 `
  logs\spectiary-profile-<timestamp>.jsonl
```

analyzer 会按 `source_load_id` 关联 summary、attempt 和 preparation round，验证连续索引、时间
单调性、阶段时长、attempt/round 聚合、retarget gap、最终 target/task/source 状态、精确
Present 终点，以及末尾 `profile_recorder_summary` 和 `dropped_events == 0`。默认任何不完整或
不一致都会返回非零；仅做事故日志诊断时可加 `-ReportOnly`，它会输出完整前缀的统计并同时列出
严格校验失败，不可作为回归 PASS。
