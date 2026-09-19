# 领域合同

这里描述现行数据边界与行为合同。产品目标见[产品需求](../product_requirements.md)，
工程操作见[开发文档](../development/README.md)，决策理由保留在 [ADR](../adr/README.md)；
历史实验和验收结果见[证据目录](../evidence/README.md)。
术语统一由根 [CONTEXT.md](../../CONTEXT.md) 定义。

| 领域 | 入口与职责 |
| --- | --- |
| 光谱数据 | [Spectra](spectra/README.md)：输入格式、源集合和不可变 snapshot 边界 |
| 样本标注 | [Labeling](labeling/README.md)：工作流、窗口交互、canonical ASDF 和持久化归属 |
| 谱线参考 | [Spectral lines](spectral-lines/README.md)：物理目录、plot overlay 与用户分组状态 |

各领域页面标明合同归属。格式字段、UI 行为和持久化生命周期分别维护，通过链接引用，
不把历史测量、实现阶段或外部库能力当成当前产品承诺。
