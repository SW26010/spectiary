# 窗口缩放：调查结论与验证

对应 [GitHub issue #56](https://github.com/SW26010/SpecForge/issues/56)。本目录集中保存本次调查；长期事件定义见 [presentation telemetry](../telemetry.md)。

## 基础观测阶段

主窗口在原生 modal size/move 期间暂停绘制，松手后更新；独立面板持续绘制但有长帧。两者需要分别解决，内部 dock 分界线是流畅性对照。当前只建立观测能力，未改变 resize/presentation 策略，issue 保持开放。

- [00:33 基线](evidence/20260911-003332-baseline.md)：主窗口更新时机与独立面板耗时；存在已知历史 teardown role 缺陷。
- [00:50 独立面板](evidence/20260911-005051-detached.md)：长帧落在 native size callback。
- [00:55 内部分界线](evidence/20260911-005523-dock-divider.md)：内部布局没有 native resize，是对照而非修复对象。

下一步细分原生 callback 的消息处理、线程等待与资源调用，证据再决定策略实验。原始采样保留在忽略的本机 logs/ 中。
