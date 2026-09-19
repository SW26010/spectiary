# Spectiary 文档

按阅读目的选择入口。产品介绍与下载入口在仓库 [README](../README.md)，
领域术语以根目录 [CONTEXT.md](../CONTEXT.md) 为准。

| 要做什么 | 从这里开始 |
| --- | --- |
| 理解产品目标与范围 | [产品需求](product_requirements.md)、[技术方向](technical_direction.md) |
| 配置环境、构建或打包 | [开发指南](development/README.md) |
| 理解数据、样本标注和谱线合同 | [领域参考](reference/README.md) |
| 执行测试、采集性能证据 | [测试指南](testing/README.md) |
| 修改显示呈现、帧调度或窗口缩放 | [呈现专题](presentation/README.md) |
| 理解架构决策及其原因 | [ADR 索引](adr/README.md) |
| 使用仓库 Agent 工作约定 | [Agent 指南](agents/README.md) |
| 查找历史实验、阶段验收与测量结果 | [证据索引](evidence/README.md) |

## 文档职责

- **现行合同**记录支持范围、不变量和字段规则。一个规则在所属专题维护，其他页面引用它。
- **操作指南**记录准备条件、命令、输出和结果判定，链接相关合同与历史基线。
- **ADR**记录决策、理由、取舍和后续修订；已有编号保持稳定。
- **历史设计、实验和证据**注明时间或来源阶段、适用范围，并链接现行入口。
  历史结果不随新实现改写，也不自动成为当前支持承诺。
- **任务进度和 PRD**在 [GitHub Issues](https://github.com/SW26010/spectiary/issues)
  维护，避免在多个文档里重复维护“下一步”或完成清单。

`presentation/live-resize/` 的设计、实验和日期证据保留在专题内，其他证据按主题放在
`evidence/`。根目录仅保留总入口、产品需求和技术方向。

## 维护导航

新增文档时，从所属专题入口链接它，并说明它是现行合同、操作指南还是历史记录。
移动或拆分文档时，同时检查 Markdown 相对链接、章节锚点，以及代码、脚本和构建配置
中的路径引用；例如 automation samples 文档本身是契约测试的输入。
