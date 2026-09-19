# 历史实验与验收证据

这里保留特定版本、环境或调查阶段的结果。每份记录说明其来源和范围；当时的
“当前”“下一步”不自动适用于之后的实现。现行入口见[文档导航](../README.md)。

| 专题 | 历史记录 | 现行入口 |
| --- | --- | --- |
| 产品路线 | [初始路线与交接稿](product/initial-roadmap.md) | [产品需求](../product_requirements.md) |
| ASDF 选型 | [representation 与 codec spike](asdf-labeling/asdf_labeling_spike.md) | [样本标注](../reference/labeling/README.md) |
| ASDF hardening | [测量报告与结构化结果](asdf-labeling-production-hardening/README.md) | [canonical schema](../reference/labeling/canonical_asdf_schema.md) |
| 目录注册 | [原生注册实验](directory-registration/README.md) | [ADR 0008](../adr/0008-directory-change-registration-lifetime-and-wait.md) |
| 存储切换 | [2026-09-19 联合验收](storage/20260919-storage-acceptance.md) | [ADR 0015](../adr/0015-application-storage-cutover.md) |
| 项目身份 | [2026-09-19 改名验收](project-identity/20260919-rename-validation.md) | [身份与改名合同](../development/project_rename.md) |
| Automation CI | [2026-09-02 范围拆分测量](automation/20260902-scope-timing.md) | [Automation CI](../development/automation/automation_ci.md) |
| UI 响应性 | [实现复盘](performance/ui-responsiveness-history.md)、[pan 历史基线](performance/pan-baselines.md) | [响应性约束](../development/ui_responsiveness.md)、[性能测试](../testing/performance_testing.md) |
| 显示呈现 | [呈现实验历史](../presentation/evidence/presentation-history.md) | [呈现合同](../presentation/policy.md) |
| 窗口缩放 | [设计、实验与日期证据索引](../presentation/live-resize/README.md#证据索引) | [窗口缩放现状](../presentation/live-resize/README.md) |

结构化结果与对应报告放在一起。大型生成产物、原始采集和本地数据仍遵循仓库忽略规则，
不因整理文档而加入版本控制。历史文件中的旧品牌、旧路径和版本号可以是测量身份的一部分，
不能把它们批量替换成今天的值。
