# 架构决策索引

ADR 保留决策理由与取舍，编号不因目录整理改变。阅读旧决策时同时看文首 amendment
和后续相关决策；后续 ADR 只替代它明确指出的范围。详细操作和字段合同从
[文档入口](../README.md)查找。

| 编号 | 决策 |
| --- | --- |
| 0001 | [Sample Labeling Architecture](0001-sample-labeling-architecture.md) |
| 0002 | [Release Profiles Are Separate Artifacts](0002-release-profile-artifacts.md) |
| 0003 | [Runtime Deployment Metadata Selects Storage](0003-runtime-deployment-metadata.md) |
| 0004 | [Lightweight Multi-Instance Runs Share User State](0004-lightweight-multi-instance-user-state.md) |
| 0005 | [Detached Panels Use Independent Viewports with Win32 Ownership](0005-detached-panel-win32-ownership.md) |
| 0006 | [Distribution Capability and Dependency Linkage Policy](0006-distribution-capability-and-linkage-policy.md) |
| 0007 | [ASDF Labeling Block Checksum Policy](0007-asdf-labeling-block-checksum-policy.md) |
| 0008 | [Directory Change Registration Lifetime and Waiting](0008-directory-change-registration-lifetime-and-wait.md) |
| 0009 | [Source Loading Dependency Boundaries](0009-source-loading-dependency-boundaries.md) |
| 0010 | [Proportionate Complexity and Maintainable Integrations](0010-proportionate-complexity-and-maintainable-integrations.md) |
| 0011 | [Explicit project identity and naming contracts](0011-project-identity-contracts.md) |
| 0012 | [Startup storage context (#103-A)](0012-startup-storage-context.md) |
| 0013 | [Spectrum-view automatic writeback authority (#110)](0013-spectrum-view-writeback-authority.md) |
| 0014 | [Spectrum preference and viewport ownership (#111)](0014-spectrum-persistence-ownership.md) |
| 0015 | [Application-managed storage physical cutover](0015-application-storage-cutover.md) |
| 0016 | [External-open instance routing](0016-external-open-instance-routing.md) |
| 0017 | [Source Jump List and startup restore policy](0017-source-jump-list-and-startup-policy.md) |

存储与身份相关决策建议结合阅读：0011 定义身份合同，0012 定义启动上下文，
0013/0014 定义 spectrum 写回与拆分 ownership，0015 定义最终物理目录切换。
