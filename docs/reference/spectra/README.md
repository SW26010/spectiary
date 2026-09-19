# Spectrum input and snapshots

| 页面 | 职责 |
| --- | --- |
| [数据格式与打开策略](data_formats.md) | CSV、NPY、FITS 与文件夹的识别、读取和领域语义 |
| [Spectrum Snapshot Contract](spectrum_snapshot_contract.md) | producer 向 UI/plot 发布的不可变快照、axis semantics、capabilities 和 diagnostics |

Loader 解析文件并产出 snapshot；UI/plot 不重新判断格式或数据语义。
异步 source loading、资源回收与本地 JSON 实现约束见
[运行时数据与状态](../../development/runtime_data.md)。
标注 ASDF 格式另由 [labeling 合同](../labeling/README.md) 定义，
谱线 overlay 使用 [spectral-line 合同](../spectral-lines/README.md)。

返回[领域合同索引](../README.md)。
