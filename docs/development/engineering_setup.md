# Spectiary 工程环境

## 环境与入口

仓库当前是 native shell + 多格式真实数据 loader。`spectiary_native` 是 Win32 + DirectX 11 executable target，
用于初始化 Dear ImGui docking、ImPlot、dock host、主图、文件区、信息/标签区、谱线区、状态栏和可选 JSONL profile sink。

本页负责工具安装、构建、测试和发布命令。相关现行合同见：

- [原生数据加载与本地 JSON 边界](runtime_data.md)。
- [Windows 平台呈现与 UI 文本合同](../presentation/platform_contract.md)。
- [Release artifacts 与 About 身份](release_artifacts.md)。
- [开发文档入口](README.md)。

## 必需工具

- Visual Studio 2022 Build Tools。
- C++ desktop workload。
- Windows 10/11 SDK。
- CMake 3.24 或更新版本。
- vcpkg。
- Ninja，可选，用于 `ninja-msvc-debug` 和发布前验证使用的
  `ninja-msvc-release-static` preset。

## vcpkg

设置 `VCPKG_ROOT`：

```powershell
[Environment]::SetEnvironmentVariable('VCPKG_ROOT', (Join-Path $env:USERPROFILE 'vcpkg'), 'User')
```

重新打开终端后确认：

```powershell
$env:VCPKG_ROOT
```

`vcpkg.json` 使用 manifest mode，当前目标依赖为：

- `imgui[docking-experimental,win32-binding,dx11-binding]`
- `implot`
- `cfitsio[pthreads]`（`default-features=false`，后台并发加载要求 reentrant build）
- `yaml-cpp`
- `nlohmann-json` (header-only; native JSON DOM, parsing and deterministic serialization)
- `zlib`

manifest 固定 `builtin-baseline`，避免依赖版本跟随本机 `VCPKG_ROOT` checkout 漂移。

DirectX 11 来自 Windows SDK；`spectiary_renderer` 封装 DX11/DXGI presentation，`spectiary_native` 负责 Win32/DWM shell。
FITS container 解析使用 vcpkg 提供的 CFITSIO。Debug preset 使用
`x64-windows`，允许 vcpkg 依赖以 DLL 形式存在；当前正式 Release preset
统一继承 `x64-windows-static`，因此 CFITSIO 链入 `Spectiary.exe`，Portable
包不携带 `cfitsio.dll`。这是当前 CFITSIO/Release 的具体选择，不是“所有依赖
必须静态”的全局规则；依赖链接策略仍按组件和分发需求分别决定。
`yaml-cpp` 用于 production ASDF sample-labeling 文档的受限 YAML metadata
解析；`zlib` 用于该 codec 的固定压缩 profile，以及受限 `.fits.gz`
单光谱读取路径。

## CMake Presets

Jump List 的原生 shell 与真实 GUI 回归使用测试程序的隐式 shell identity／临时 Portable 目录，
不会改动用户的 Files roster；后者会短暂打开并关闭测试窗口，其任务栏分组与
Jump List 生命周期由 Windows 决定，不承诺按配置目录隔离。构建后可显式运行：

```powershell
ctest --test-dir build/ninja-msvc-debug -R '^spectiary_win32_jump_list_(native|gui_routing)_tests$' --output-on-failure
```

它们标记为 `extended;real-gui;gui-integration`，不随 headless `extended` preset
运行；确定性的发布协议测试属于 `fast`。启动和 shell identity 合同见
[ADR 0017](../adr/0017-source-jump-list-and-startup-policy.md)。

GUI 回归也验证首次无来源启动恢复、后续无来源启动为空实例、空实例关闭不覆盖会话，
以及显式 Jump List 来源仍保留完整 Files 清单。真实 Windows 手势另做人工验收：
保存来源后退出全部实例，从开始菜单普通启动确认恢复；在其运行时用任务栏 Shift-click
或 Win+Shift+数字确认新窗口为空。再退出全部实例，测试冷启动 Start Menu + Shift；
如果 Windows 产生两次 invocation，确认首次恢复、后续为空。该手势时序不由进程启动测试替代；
现有 endpoint 只在 GUI 初始化后注册，不为未复现的并发启动增加存在性锁。

共享配置写在 `CMakePresets.json`。本地个人配置写在 `CMakeUserPresets.json`，不要提交。

Ninja configure check：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\build-ninja-msvc-debug.ps1 -Configure
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\build-ninja-msvc-debug.ps1 -Preset ninja-msvc-release-static -Configure
```

Visual Studio configure check：

```powershell
cmake --preset vs2022-x64-debug
```

Configure success 验证依赖和生成文件，build success 验证 native shell target。

Build native shell：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\build-ninja-msvc-debug.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\build-ninja-msvc-debug.ps1 -Preset ninja-msvc-release-static
```

### CTest 验证层级

完成 `ninja-msvc-debug` configure 并通过上面的包装器构建所需测试 target 后，日常开发的标准
edit/test loop 是从仓库根目录运行：

```powershell
ctest --preset fast
```

`fast` 是确定性、低成本的日常开发反馈层。需要在提交前执行更完整但仍为 headless 的开发验证时，运行：

```powershell
ctest --preset extended
```

两个 preset 为未单独声明超时的测试提供 120 秒默认上限；测试自己的 CTest
`TIMEOUT` 属性仍优先。这是卡住时的退出边界，不是整套测试的耗时预算。

已测耗时与适用边界见 [2026-09-02 CI 范围与 fast tier 记录](../evidence/automation/20260902-scope-timing.md#repository-fast-tier)。

`extended` 会运行较慢的 headless 测试，但不会代替需要专门环境或目的的验证。验证入口分工如下：

| 入口 | 用途 | 选择边界 |
| --- | --- | --- |
| `fast` preset | 日常开发 | 快速、确定性的 unit、component 和廉价 contract 测试 |
| `extended` preset | 更完整 headless | 较慢的 headless 开发验证；排除下列专门验证 labels |
| `real-gui` label | 专门 GUI 验证 | 需要交互式 Windows desktop；`gui-integration` 也按同一专门边界处理 |
| `release` label | 专门发布验证 | release artifact、packaging、configured-build 等发布合同 |
| `periodic` label | 专门周期验证 | 显式启用的资源稳定性 smoke/soak 工作负载 |
| `asdf-pinned-oracle` label | 专门 ASDF oracle 验证 | 需要 dedicated ASDF preset、固定 Python ASDF 版本和 oracle 依赖 |

`fast` / `extended` 是每项测试恰好选择一个的执行成本 tier；专门 labels 是与之正交的验证责任。
因此廉价 component test 可以同时属于 `fast` 和 `release`，但完整 release suite 仍必须通过 `release`
label 显式运行。`extended` preset 会排除 `real-gui`、`gui-integration`、`release`、`periodic` 和
`asdf-pinned-oracle`，不会把这些专门验证静默计入普通 headless 结果。未限定的 aggregate `ctest`
不是日常开发入口。

专门验证应在满足相应 build、desktop、数据或 oracle 前置条件后，以对应 label 显式选择：

```powershell
ctest --test-dir <build-directory> --output-on-failure --no-tests=error -L '^<label>$'
```

真实 GUI 的 runner 与证据规则见[自动化 CI](automation/automation_ci.md)，release 构建与 artifact 合同见
[Release artifacts](release_artifacts.md)，periodic 资源稳定性 tier 见[响应速度测试流程](../testing/performance_testing.md)，
ASDF pinned oracle 的环境与命令见[ASDF labeling spike 运行说明](../../tools/asdf_labeling_spike/README.md)。

### Ninja/MSVC 卡住排查

`ninja-msvc-debug` 和 `ninja-msvc-release-static` preset 依赖 MSVC developer
environment 与 vcpkg manifest mode。不要在普通 PowerShell 里裸跑 `ninja` 或
直接执行对应的 `cmake --build --preset ...`；`cl.exe` 可能找不到标准库头，例如
`cstddef`。

在 Codex 或其它只允许写仓库目录的受限环境里，configure/build 需要用同一套 `vcvars64.bat` 命令形态并允许写
workspace 外缓存。这两个 Ninja preset 的 configure 都会调用 vcpkg，并可能写入
`$env:VCPKG_ROOT\buildtrees\0.vcpkg_dep_info.cmake`、`buildtrees/`、`packages/`、下载缓存或 MSVC
工具链缓存；如果沙箱拦住这些 workspace 外写入，表现可能是 configure 失败或后续 build 看起来卡住。

受限环境中的正确处理方式是把 configure 和 build 都作为需要外部工具链/cache 写入权限的命令执行，并优先使用
`scripts/build-ninja-msvc-debug.ps1`。这个脚本会加载 `vcvars64.bat`、记录 stdout/stderr 到 `logs/build/`，
并把构建进程放进 Windows Job Object。启动真正的 CMake 之前，脚本会先做一次 process-kill preflight：
创建一个短生命周期探针进程、放进同类 job，然后验证当前 shell 能用 `taskkill /T /F` 清掉这棵进程树。
如果 preflight 失败，脚本会在进入 CMake 前直接失败；在 Codex 里这表示普通沙箱不能自救，需要用提权/非沙箱
方式重跑，而不是重复等待同一个命令。

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\build-ninja-msvc-debug.ps1 -Configure -TimeoutSec 180
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\build-ninja-msvc-debug.ps1 -Target spectiary_source_collection_session_tests -TimeoutSec 60 -Explain
```

如果看到类似 `This shell cannot kill job-assigned process trees with taskkill /T /F` 的 preflight 错误，
不要继续在同一个普通受限 shell 里重试。对 Codex agent 来说，这个命令必须通过 sandbox escalation 执行，
因为 vcpkg/MSVC 会写 workspace 外缓存，且超时清理需要能终止 job-assigned `cmd/cmake/ninja` 进程树。

如果一次构建被中断，后续命令可能卡在 Ninja lock。先查是否有残留构建进程：

```powershell
Get-Process | Where-Object { $_.ProcessName -match 'cmake|ninja|cl|link|ctest|msbuild' } |
  Select-Object Id,ProcessName,CPU,StartTime,Path
```

确认是旧的 `cmake.exe`/`ninja.exe` 持锁后，只终止对应进程，再重跑上面的 `vcvars64.bat` configure/build 命令：

```powershell
Stop-Process -Id <cmakeId>,<ninjaId> -Force
```

生成程序位于：

```text
build/ninja-msvc-debug/Spectiary.exe
build/ninja-msvc-release-static/Spectiary.exe
```

普通构建输出的 `spectiary_metadata.json` 不含 deployment，因此 EXE 作为 Standalone 运行，ImGui layout、
panel 显示状态、profile 设置和默认日志分别写入 `%LOCALAPPDATA%\Spectiary` 下按 `config/`、`state/`、`logs/` 分工的文件或目录。
Release 程序可在 `Settings > Diagnostics` 开始/停止性能诊断录制，并可选择 profile 输出目录。
设置 `SPECTIARY_PROFILE=1` 则从启动阶段自动录制；`SPECTIARY_PROFILE_DIR` 仍可为自动化流程覆盖 UI 设置。
录制器使用有界异步写入，单次 5 分钟或 100 MiB 自动停止；分析前检查
`profile_recorder_summary.dropped_events == 0`。`scripts/analyze-profile.ps1` 默认强制检查 summary 位于日志
末尾、停止原因有效且没有丢事件；旧格式日志只有显式传入 `-AllowLegacyIncompleteRecording` 才可继续分析。

## Portable release

第一版 portable 是 no-launcher 包：zip 根目录包含 `Spectiary.exe`、
`spectiary_metadata.json` 和 `config/`、`state/`、`logs/`、`unsaved/`。第三方声明与数据来源内嵌在所有分发形式
共用的 EXE 中，可从 About 阅读。直接从当前工作区文件构建：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\build-portable.ps1
```

严格忽略 tracked dirty 和 untracked 文件、从构建开始时的本地完整 `HEAD` object ID 构建：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\build-portable-from-head.ps1
```

HEAD 脚本先解析并冻结完整 `HEAD` object ID，再用 `git archive` 将该精确 revision
展开到经过校验的临时目录，并调用快照内同一份 `build-portable.ps1`。成功或失败都会
清理临时源码和 build 目录，不修改当前工作区或 Git index。

当前工作区 builder 直接调用 CMake 和当前统一的 Visual Studio release preset。HEAD
包装器不把工作区的 preset 或 configuration 默认值下传到快照；这些默认值以及 metadata
和包内容校验均由快照内 builder 按其自身代际负责。外层只确认子进程成功且 package
目录、ZIP 和 SHA-256 文件存在，避免工作区脚本解释旧代 schema。两个入口都预期在正常
开发 shell 或已批准的非沙箱 agent 运行中执行；它们不复用 Ninja debug wrapper 的日志、
timeout 和 preflight 形态。

Working-tree 默认输出位于 `dist\Spectiary-portable`，包名来自 identity 契约中的
`artifact_basename`；HEAD 入口目前仍默认输出到 `dist\head\Spectiary-portable`。
各自的 ZIP 和 `.sha256` 位于对应的 `dist\` 或 `dist\head\` 目录，HEAD
构建不会删除或覆盖 working-tree 包。
共同脚本的 source mode/revision 参数是两个正式入口之间的内部契约；为避免 dirty
checkout 被误标为 HEAD，它在源码根仍包含 `.git` 时拒绝 `head` 模式。

包内容、metadata 字段、依赖声明、校验顺序与失败行为统一见
[Release artifacts](release_artifacts.md#portable-packaging-and-verification)。
运行身份和数据根选择见 [运行时数据边界](runtime_data.md#运行身份与存储边界)。

## 仓库卫生

文本换行由 `.gitattributes` 固定为 LF，编辑器的缩进与文件末尾换行由
`.editorconfig` 约定。Markdown 保留有意义的行末空格；ASDF fixture 和图标
保持原始字节，尤其不得重新编码或格式化带 manifest 校验和的 ASDF 文件。

以下内容不进入提交：

- `build/`
- `out/`
- `.vs/`
- `vcpkg_installed/`
- `CMakeUserPresets.json`
- 运行包中的 managed role 目录 `config/`、`state/`、`logs/`、`unsaved/` 及其运行时数据；不包括源码仓库中包含 `project_identity.json` 等正式输入的 `config/`。
- `imgui.ini`
- `logs/`
- 本地光谱数据。
- `.scratch/` 中的旧 demo 和实验。

如果后续需要测试数据，只提交小型、明确授权的 fixture，并放在专门的 fixture 路径中。

本地构建备份和实验材料集中保留在已忽略的 `.scratch/`，采样日志保留在
`logs/`。这些目录不属于 Git 提交范围，也不因一次卫生整理而自动删除。
归档或清理前应先确认相关任务不再需要该份构建，且报告引用的原始采样、
EXE/PDB 和哈希仍有可追溯副本；不要用 `git clean -xfd` 批量清除证据。

## 后续实现检查

继续实现前先确认：

- 新输入是否继续从 producer 侧产出 `SpectrumSnapshotHandle`，不让 UI/plot 解析 loader 细节。
- profile JSONL schema 是否覆盖本次要判断的交互路径。
- 真实性能判断是否使用真实 `.npy` 数据和新生成日志。
- 真实数据路径是否仍留在仓库外。

## ImGui widget regression tests

See [ImGui widget regression tests](../testing/widget_testing.md) for the headless real-widget harness, focused commands, and its boundary with business automation and presentation diagnostics.

Live-resize 的 instrumentation event、字段和机器验收见 [presentation telemetry](../presentation/telemetry.md)。这些诊断不改变 resize/presentation policy。#56 的范围、实验结论与下一步统一见 [调查入口](../presentation/live-resize/README.md)。
