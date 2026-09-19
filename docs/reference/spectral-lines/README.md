# Spectral lines

| 页面 | 职责 |
| --- | --- |
| [Spectral Line Catalog Contract](spectral_line_catalog_contract.md) | 物理坐标、公开 TSV、UI 提供的标记、plot overlay 和 label placement |
| [Spectral Line Grouping Views](spectral_line_grouping_views.md) | 用户分组、marker references、visibility、JSON state 与并发提交 |

Catalog 拥有谱线定义；用户分组保存引用和用户状态，不复制或改写物理定义。
并发写入是 [ADR 0004](../../adr/0004-lightweight-multi-instance-user-state.md)
规定的有限例外，不是所有本地状态的通用 merge 策略。

返回[领域合同索引](../README.md)。
