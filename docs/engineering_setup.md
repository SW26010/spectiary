# SpecForge 工程环境

## 当前提交范围

仓库当前是 native shell + 多格式真实数据 loader。`specforge_native` 是 Win32 + DirectX 11 executable target，
用于初始化 Dear ImGui docking、ImPlot、dock host、主图、文件区、信息/标签区、谱线区、状态栏和可选 JSONL profile sink。
默认启动仍有 small synthetic fixture 用于 smoke test；命令行源路径和 Files 面板 `Add file...` 支持通过 domain snapshot loader 打开 source。
Files 面板 `Add folder...` 使用 Windows 原生目录选择器添加目录 source，目录本身仍交给 domain snapshot loader 处理。
当前可绘制的真实数据包括 `.npy` 光谱矩阵、简单波长/流量 `.csv`、可识别的单条 LAMOST/SDSS FITS table 光谱，以及第一层包含 CSV/FITS 文件的 folder collection；受限 image FITS fallback 不作为主支持承诺，catalog/unsupported FITS 由 domain 产出不可绘制的 diagnostic snapshot。
Folder source 非递归加载第一层 CSV/FITS 文件，子文件夹、其它文件类型、CSV/FITS 混用都会写入 warning diagnostics。
`.npy` loader 支持 1D 或行级 2D float32/float64 array，CSV/FITS loader 产出同一类 `SpectrumSnapshotHandle` 进入同一条 UI/plot 路径。
3909 列矩阵使用固定 loglam wavelength grid，其他列数退回 pixel index 并写入 snapshot diagnostics。
辅助数组如 `*_label.npy`、`*_index.npy`、`*_ormask.npy` 和 `*_known_mask.npy` 不作为光谱打开。
当前仍不从 shell 或竖切片得出真实数据性能结论。

## 必需工具

- Visual Studio 2022 Build Tools。
- C++ desktop workload。
- Windows 10/11 SDK。
- CMake 3.24 或更新版本。
- vcpkg。
- Ninja，可选，仅用于 `ninja-msvc-debug` preset。

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
- `zlib`

manifest 固定 `builtin-baseline`，避免依赖版本跟随本机 `VCPKG_ROOT` checkout 漂移。

DirectX 11 来自 Windows SDK，`specforge_native` 显式链接 `d3d11`、`dxgi`、`dwmapi` 和 `imm32`。`zlib` 只用于受限 `.fits.gz` 单光谱读取路径。

## UI 文本编码与字体

SpecForge 的 UI 字符串边界是 UTF-8。业务状态、JSON cache、sample name、label name、annotation display name 和
ImGui widget buffer 都应继续使用 UTF-8 `std::string`；不要为了 Windows 输入把这些字段改成本地 ANSI code page 或在
业务层传播 `std::wstring`。

Win32 shell 必须继续使用 `RegisterClassExW`、`CreateWindowExW`、`DefWindowProcW` 和 Dear ImGui 的 Win32 backend，
让中文输入先以 Unicode 进入 ImGui，再由 ImGui 写入 UTF-8 buffer。中文显示依赖 ImGui font atlas 覆盖 CJK glyph。
native app 启动时先加载 ImGui 默认字体作为主字体，保留英文和 ASCII 的默认视觉；然后从 Windows Fonts 目录合并
系统 CJK 字体作为 fallback，例如 `NotoSansSC-VF.ttf`、`Deng.ttf`、`simhei.ttf`、`msyh.ttc` 或 `simsun.ttc`。
如果输入后的字符显示为 `?`，先检查 runtime profile 中的 `dpi_config.ui_font`，确认是否实际合并了 CJK 字体；
不要通过把 UTF-8 文本转成本地代码页来修。

## CMake Presets

共享配置写在 `CMakePresets.json`。本地个人配置写在 `CMakeUserPresets.json`，不要提交。

Ninja configure check：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\build-ninja-msvc-debug.ps1 -Configure
```

Visual Studio configure check：

```powershell
cmake --preset vs2022-x64-debug
```

Configure success 验证依赖和生成文件，build success 验证 native shell target。

Build native shell：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\build-ninja-msvc-debug.ps1
```

### Ninja/MSVC 卡住排查

`ninja-msvc-debug` preset 依赖 MSVC developer environment 和 vcpkg manifest mode。不要在普通 PowerShell
里裸跑 `ninja` 或 `cmake --build --preset ninja-msvc-debug`；`cl.exe` 可能找不到标准库头，例如 `cstddef`。

在 Codex 或其它只允许写仓库目录的受限环境里，configure/build 需要用同一套 `vcvars64.bat` 命令形态并允许写
workspace 外缓存。`cmake --preset ninja-msvc-debug` 会调用 vcpkg，并可能写入
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
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\build-ninja-msvc-debug.ps1 -Target specforge_source_collection_session_tests -TimeoutSec 60 -Explain
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
build/ninja-msvc-debug/SpecForge.exe
```

ImGui layout 写入 `imgui.ini`。设置 `SPECFORGE_PROFILE=1` 后启动程序，运行时 profile JSONL 默认写入
`logs/`；需要指定输出位置时，设置 `SPECFORGE_PROFILE_DIR`。

## 仓库卫生

以下内容不进入提交：

- `build/`
- `out/`
- `.vs/`
- `vcpkg_installed/`
- `CMakeUserPresets.json`
- `imgui.ini`
- `logs/`
- 本地光谱数据。
- `.scratch/` 中的旧 demo 和实验。

如果后续需要测试数据，只提交小型、明确授权的 fixture，并放在专门的 fixture 路径中。

## 后续实现检查

继续实现前先确认：

- 新输入是否继续从 producer 侧产出 `SpectrumSnapshotHandle`，不让 UI/plot 解析 loader 细节。
- profile JSONL schema 是否覆盖本次要判断的交互路径。
- 真实性能判断是否使用真实 `.npy` 数据和新生成日志。
- 真实数据路径是否仍留在仓库外。
