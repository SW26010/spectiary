# 运行期资源稳定性测试

[性能测试入口](../performance_testing.md) · [测试导航](../README.md)

## 工作负载与观测范围

`scripts/test-runtime-resource-stability.ps1` 是进程级资源门禁。它启动真实
`Spectiary.exe`，由应用内的受控工作负载执行 source 重复加载、同路径突发替换/取消、不同
source 切换、窗口 resize、基线恢复和正常关闭；外部 runner 同时采样：

- Private Bytes（进程 private commit）；
- Working Set 和 Virtual Bytes；
- Win32 handle、thread、GDI object、USER object 数量；
- source load queue 的成功取消、待处理 completion、残留 worker，以及后台 retirement
  排队/在途/累计完成状态；
- profile writer 的唯一末尾 `profile_recorder_summary`、`stop_reason=explicit`
  和 `dropped_events == 0`；
- Windows Graphics Tools 可用时，D3D11 debug layer 的关闭期 live-object 明细
  （包含 D3D11 child object 和 swap-chain 残留检查）。

进入 warm-up 前，workload 会逐个加载并 drain 所有配置 source，最后回到第一个 source 和固定
1280×820 窗口；这会先填满 roster 的正常 per-source cache。每个正式循环也以该 baseline source
及已 drain 的 load/retirement 队列收尾，再经过 settle 窗口取样。因此趋势比较的是同一运行状态，
而不是首次缓存新 source 或比较两个大小不同的 source。warm-up settle 完成后才重置取消/退休活动
基线；每个 measured cycle 都必须各自产生成功取消和后台退休，否则即使 warm-up 有过活动也会失败。
取消证据由一次性的 worker checkpoint 控制：首个 stress 请求进入准备阶段前先与 workload 握手，
后续突发请求再确定性取消它，因此 resident-reuse 快路径也不依赖线程调度恰好足够慢。
stress 与 baseline 加载完成后还不能直接推进：workload 必须观察到该次 source-load 对应的
精确 sample snapshot 已提交绘制，并由对应 viewport 成功 Present；同时该阶段请求的
render-target resize 必须已经实际执行。每个 measured cycle 会把两段的 source identity、
source-load ID、activation frame、Present/resize sequence 写入状态文件，runner 再逐项与
配置 source 轮转及 profile 中的 `outcome=presented` ID 交叉核对。窗口最小化或隐藏后没有
成功 Present 时，状态机会停留在 `wait_*_presented` 并按 phase timeout 失败，warm-up
期间的历史 Present 不能替代正式循环证据。
Working Set 仍输出，但不会单独触发泄露失败；Windows 文件缓存和工作集修剪会使它在没有 private
commit 泄露时上下波动。

受控 workload 仍保留 ProfileSink 的有界队列和文件大小限制，但将内部录制时长上限设为 `0`，
由 runner 的 `TimeoutSec` 统一负责外部时限；profile 起始事件中的 `max_duration_seconds` 必须为
`0`，最终仍要求 `stop_reason=explicit`。这避免合法的 `soak` 在普通 5 分钟录制上限处固定假红。

## 构建与运行

fresh checkout 先按仓库约定用 wrapper 配置，再单独构建目标（`-Configure`
只执行配置，不同时构建）：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass `
  -File scripts\build-ninja-msvc-debug.ps1 `
  -Configure `
  -TimeoutSec 600

powershell -NoProfile -ExecutionPolicy Bypass `
  -File scripts\build-ninja-msvc-debug.ps1 `
  -Target spectiary_native `
  -TimeoutSec 600
```

然后显式选择至少两个稳定、可重复读取的真实 source。第一个是每轮恢复后的比较基线，其余 source
轮流承担 stress load：

```powershell
& scripts\test-runtime-resource-stability.ps1 `
  -Executable build\ninja-msvc-debug\Spectiary.exe `
  -Source @(
    'D:\spectra\stable-baseline.npy',
    'D:\spectra\stable-stress.fits'
  )
```

如果通过 `powershell -File`、CTest 或其他不便传递 PowerShell 数组的调用器启动，可提供只含
JSON 字符串数组的 `-SourceListFile`，或设置：

```powershell
$env:SPECTIARY_RESOURCE_STABILITY_SOURCES_JSON = ConvertTo-Json @(
  'D:\spectra\stable-baseline.npy',
  'D:\spectra\stable-stress.fits'
) -Compress
$env:SPECTIARY_RESOURCE_STABILITY_TIER = 'smoke'

ctest --test-dir build\ninja-msvc-debug `
  -R '^spectiary_runtime_resource_stability$' `
  --output-on-failure

Remove-Item Env:SPECTIARY_RESOURCE_STABILITY_SOURCES_JSON
Remove-Item Env:SPECTIARY_RESOURCE_STABILITY_TIER
```

## 测试层级

真实 GUI CTest 只有显式设置 `SPECTIARY_RESOURCE_STABILITY_TIER` 后才启用；仅配置 source 也仍会
skip。tier 已启用但 source 缺失时则 fail closed，避免定期任务因配置错误得到绿色 skip。

| 层级 | 循环 | 默认 CTest 行为 | 用途 |
| --- | ---: | --- | --- |
| workload 状态机单测 | 1 warm-up + 1 measured（模拟时钟/资源） | 始终执行 | 无 GUI、无真实 source，确定性覆盖完整成功、Present/resize、收尾和 close；通常低于 0.1 秒 |
| runner contract + 反例 | 无正式循环；两个短进程反例 | 始终执行 | 状态共享、趋势分析、逐周期 Present 证据，以及最小化/首次状态写失败回归；当前约 3–4 秒 |
| `smoke` | 2 warm-up + 12 measured | 显式启用 | 约十几秒，检查生命周期收尾和明显的资源增长 |
| `soak` | 5 warm-up + 100 measured | 显式启用 | 通常约一至数分钟，定期检查更慢的持续趋势 |

不设置 tier 时，带 `resource-stability;periodic` label 的真实 GUI 项约在启动 runner 后立即返回
skip；普通 CTest 承担快速的 `spectiary_runtime_resource_workload_tests` 和
`spectiary_runtime_resource_stability_runner_tests`。数据加载时间仍取决于 source，因此表中
只是量级，不是超时合同。建议普通提交不启用 tier；高风险 UI/加载改动可显式跑 `smoke`，
`soak` 由定期任务执行。直接调用脚本而不加 `-CTestIntegration` 时仍是可调参数的 `custom` 层级。

## 采样与趋势门禁

runner 默认执行 2 个 warm-up 和 12 个 measured 循环。它对 measured 循环的 post-settle 样本做
线性回归；只有当增长量、每循环 slope 和 `R² >= 0.50` 同时超过门限时，才判为持续线性增长。
`SampleIntervalMs` 至少为 50 ms，且不得超过 `SettleMs` 的一半，保证每个 settling 窗口至少有明确
采样余量。每次进程资源读取都夹在两次 workload status 读取之间；只有 `state`、`phase`、
`completed_cycles` 和 `completed_measured_cycles` 在前后完全一致时才接纳该样本，避免阶段切换
期间的新资源值被标成上一轮 settling。被丢弃的边界样本数写入结果的
`sampling.discarded_transition_sample_count`。
默认门限为：

| 指标 | 最大增长量 | 最大 slope / cycle |
| --- | ---: | ---: |
| Private Bytes / commit | 16 MiB | 1 MiB |
| handles | 4 | 0.25 |
| threads | 2 | 0.10 |
| GDI objects | 4 | 0.25 |
| USER objects | 4 | 0.25 |

这些是自动化的保守起点，不是跨机器常数。换用显著不同的数据规模、驱动或 allocator 后，应先保留
新证据，以 `-ReportOnly` 观察稳定平台，再显式调整对应 `-Max...` 参数；不要因为单次峰值或
Working Set 上涨而放宽 private commit/handle/thread 门禁。

## 结果产物与失败处理

直接调用脚本且不指定 `-OutputDirectory` 时，证据写入
`logs/runtime-resource-stability/<UTC timestamp>/`。通过仓库 CTest 启动时，CMake 显式改为
`<build directory>/runtime-resource-stability/<UTC timestamp>/`；例如 Ninja/MSVC Debug 构建为
`build/ninja-msvc-debug/runtime-resource-stability/<UTC timestamp>/`。每次运行生成：

- `result.json`：最终 `PASS`/`FAIL`、每项 check、阈值、source 元数据和证据路径；
- `samples.csv`：连续进程资源样本及对应 workload phase/cycle；
- `workload-config.json` / `workload-status.json`：应用内驱动合同和最终 drain/取消/退休证据；
- `spectiary-profile-*.jsonl`：加载取消、present、shutdown 和 writer summary。
- `state/runtime-resource-startup-error.txt`：仅在自动 workload 启动异常时生成，进程直接非零退出，
  不弹阻塞 CI 的模态框。

任一强制 check 失败时 runner 返回非零；`-ReportOnly` 仍写完整结果但不把观察性失败转成脚本失败。
若安装了 Windows Graphics Tools，图形 live-object check 自动参与门禁；不可用时记录 `SKIP` 和
HRESULT。要求该检查必须可用时加 `-RequireGraphicsDiagnostics`。runner 通过仅在工作负载环境存在时
生效的隔离状态目录启动应用，不读取或写入普通用户的 Spectiary session/settings。
