# 开发与工程操作

这些文档记录现行开发入口与合同。日期验收和性能观察保存在 [evidence](../evidence/README.md)，
产品与技术方向从 [文档总入口](../README.md) 进入。

| 目的 | 文档 |
| --- | --- |
| 安装工具、配置、构建、选择测试与排障 | [工程环境](engineering_setup.md) |
| 构建 working-tree 或隔离 HEAD Portable 包 | [Portable release 命令](engineering_setup.md#portable-release) |
| 校验 schema 6 metadata、制品与 About 身份 | [Release artifacts](release_artifacts.md) |
| 修改数据加载、本地 JSON 或运行时存储边界 | [原生数据与本地状态](runtime_data.md) |
| 排查或修改 UI 等待与后台任务 | [UI responsiveness](ui_responsiveness.md) |
| 理解项目身份、兼容边界与未来改名步骤 | [Project identity](project_rename.md) |
| 启动和验证本地 automation | [Automation](automation/README.md) |
| 修改 Win32/DXGI、帧唤醒或 UI 文本 | [平台呈现与文本合同](../presentation/platform_contract.md) |

Ninja/MSVC configure 与 build 必须通过仓库的
`scripts/build-ninja-msvc-debug.ps1` 包装器；受限 agent 环境需要 sandbox escalation。
具体命令和进程清理边界见 [Ninja/MSVC 排障](engineering_setup.md#ninjamsvc-卡住排查)。

Automation workflow 保持 `workflow_dispatch` 手动触发；release 的版本 tag
触发规则不授权其他 workflow 增加自动触发。完整执行与 runner 规则见
[Automation CI](automation/automation_ci.md#manual-execution-policy)。
