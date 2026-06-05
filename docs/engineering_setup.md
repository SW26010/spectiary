# SpecForge 工程环境

## 当前提交范围

这是第一次提交前的环境整理阶段。仓库当前只验证依赖声明和工程方向，不声明应用 executable target。

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

DirectX 11 来自 Windows SDK，后续 executable target 应链接 `d3d11` 和 `dxgi`。

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

当前阶段 configure success 即为环境验收。没有 executable target，也没有可运行程序。

## 仓库卫生

以下内容不进入提交：

- `build/`
- `out/`
- `.vs/`
- `vcpkg_installed/`
- `CMakeUserPresets.json`
- `imgui.ini`
- 本地光谱数据。
- `.scratch/` 中的旧 demo 和实验。

如果后续需要测试数据，只提交小型、明确授权的 fixture，并放在专门的 fixture 路径中。

## 下一次实现前检查

开始写代码前先确认：

- Win32 + DX11 shell 是否直接以 Dear ImGui official example 为基础。
- docking 是否通过标准 dockspace 实现。
- 第一条 spectrum 数据从哪个输入合同进入。
- profile JSONL schema 是否先于性能判断落地。
- 真实数据路径是否仍留在仓库外。
