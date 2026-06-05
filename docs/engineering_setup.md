# SpecForge 工程环境

## 当前提交范围

仓库已经进入 Milestone 1 的 native shell 阶段。当前声明一个 Win32 + DirectX 11 executable target，
用于初始化 Dear ImGui docking、ImPlot、dock host、主图、文件区、信息/标签区、谱线区、状态栏和可选 JSONL profile sink。
当前 shell 只使用 small synthetic fixture，不从它得出真实数据性能结论。

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
[Environment]::SetEnvironmentVariable('VCPKG_ROOT', 'C:\dev\vcpkg', 'User')
```

重新打开终端后确认：

```powershell
$env:VCPKG_ROOT
```

`vcpkg.json` 使用 manifest mode，当前目标依赖为：

- `imgui[docking-experimental,win32-binding,dx11-binding]`
- `implot`

manifest 固定 `builtin-baseline`，避免依赖版本跟随本机 `VCPKG_ROOT` checkout 漂移。

DirectX 11 来自 Windows SDK，`specforge_native` 显式链接 `d3d11`、`dxgi`、`dwmapi` 和 `imm32`。

## CMake Presets

共享配置写在 `CMakePresets.json`。本地个人配置写在 `CMakeUserPresets.json`，不要提交。

Ninja configure check：

```powershell
cmd.exe /d /c "call ""C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"" && cmake --preset ninja-msvc-debug"
```

Visual Studio configure check：

```powershell
cmake --preset vs2022-x64-debug
```

Configure success 验证依赖和生成文件，build success 验证 native shell target。

Build native shell：

```powershell
cmd.exe /d /c "call ""C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"" && cmake --build --preset ninja-msvc-debug"
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

- 第一条 spectrum 数据从哪个输入合同进入。
- profile JSONL schema 是否先于性能判断落地。
- 真实数据路径是否仍留在仓库外。
