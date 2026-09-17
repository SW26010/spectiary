# 普通窗口样式 A：增量 buffer 替换复验

用户反馈：对照“明显卡顿”，增量组“几乎观察不到卡顿”。本机普通窗口样式下的候选收益得到正向证据；资源稳定性、跨环境支持和生产启用尚未验收。

## 身份与完整性

- 对照：`logs/live-resize-20260912-140009-f73e760c`。
- 增量组：`logs/live-resize-20260912-140108-043c23b1`。
- 两组 EXE SHA256 相同：`0E5F6F947892F61854EC7575436487BE583CBB6C1EFABD0A195CF79FD1AB2FCF`。不同于 9 月 11 日 B 组构建，不能跨日期合并统计。
- 两组均为普通 A 样式、原有 feedback 频率；实际 incremental 开关为 false/true。
- 完整性、detached/native-size/feedback coverage 和 A 样式校验通过；增量组额外通过替换策略校验，无 DXGI fallback。
- 均以 duration_limit 正常收尾，窗口采样边界配对，零丢事件。accepted_bytes 分别为 40,569,994 / 38,079,775。
- 两组各有两个 detached viewport。尺寸变化只属于 lifetime 2、viewport ID 167297964；另一个保持 860×560。以下阶段仅统计目标 viewport，整帧包含所有窗口工作。

## 性能与行为

| 指标 | 对照 | 增量组 |
| --- | ---: | ---: |
| 目标 resize 帧 | 256 | 226 |
| 整帧 p50 | 13.5901 ms | 12.2146 ms |
| 整帧 p95 | 17.5272 ms | 13.7545 ms |
| 整帧 p99 | 20.9514 ms | 14.1725 ms |
| 整帧最大值 | 22.7930 ms | 14.6985 ms |
| native size callback p95 | 3.0412 ms | 2.0661 ms |
| viewport_resize p95 | 5.8949 ms | 0.0835 ms |
| acquisition p95 | 0.2906 ms | 1.4016 ms |
| Present p95 | 2.4288 ms | 2.9232 ms |
| 单槽位 release p95 | 2.2247 ms | 0.3818 ms |
| 单槽位 release 最大值 | 4.5749 ms | 0.5152 ms |

Nearest-rank 分位数。此配对整帧 p95 约降低 21.5%，p99 约降低 32.4%。Resize 变短本身不算收益：工作移到了 acquisition，Present p95 也增加；整帧长尾和主观体验同时改善才支持继续推进。

对照释放 768 个槽位（每 resize 帧三个）；增量组 226 个 resize 帧各成功替换一个，槽位 0/1/2 分别替换 96/68/62 次。替换 p95 为 0.7106 ms，最大 0.8272 ms。目标 acquisition/Present 全部返回 S_OK，无 still-drawing skip。完整增量采样有 395 次 select、229 次 replace，无 skip/budget_failure。resize 帧内计划逻辑纹理峰值 45,619,568 字节，低于每 viewport 256 MiB 预算；不能以此代替进程/GPU 内存实测。

手工路径和初始尺寸不同：对照宽/高范围 582..2044 / 771..1381，增量组 351..2134 / 600..1551。短样本不证明普遍固定幅度的收益；native callback 差异不能全部归因于 buffer 策略。本次无 ETL，不据旧记录断言新长帧的内核等待。S_OK 是 API 提交成功，不是物理显示完成。

## 当前决策与维护范围

已读取 #56 最新正文（updatedAt `2026-09-12T05:38:58Z`，无评论）。#56 仅负责独立面板卡顿；主窗口原生拖拽刷新归 #101，不阻塞这里的验收。

普通 A 样式复验为正，下一步转入有限回归，不重复相同 A/B，也不扩展优化机制：

1. 复用已有 D3D/viewport 测试，补齐反复尺寸振荡、销毁重建、预算失败及回退的实际覆盖缺口。
2. 在有限交互中观察进程/GPU 内存和句柄趋势，区分热身缓存与持续增长；逻辑预算不代替实测。
3. 检查多个独立面板、内部 dock 分界线、停靠/拆出和现有 ownership、最小化/恢复行为。
4. 按 ADR 0010 审查生产必要性，仅保留有收益的局部资源策略及必要观测。重定向 backend 补丁、反馈频率实验、专用 hook/开关须说明保留或移除理由，不能整体打包启用。

复用官方 API 和已有生命周期/回退，不引入新渲染线程、窗口系统替代或驱动特例。通过有限检查后再整理生产候选；本次不启用默认策略、不关闭或修改远端 issue。

产物：两目录的 `validation.json`、`incremental-comparison.json`。后者由既有 `logs/compare-incremental-buffers.cjs` 按发生 resize 的 lifetime 计算。原始日志未改动。