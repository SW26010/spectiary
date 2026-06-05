# SpecForge

SpecForge is being prepared as a native Windows spectrum viewer built on Win32,
DirectX 11, Dear ImGui docking, ImPlot, CMake, and vcpkg.

This repository now contains the first native shell slice for the product
direction: a Win32 + DirectX 11 executable with Dear ImGui docking, ImPlot, a
synthetic spectrum fixture, and optional JSONL profile output.

## Current State

- `specforge_native` builds the Windows executable target.
- The app shell initializes Win32, DirectX 11, Dear ImGui docking, and ImPlot.
- The main plot uses a small synthetic fixture only; no real-data performance
  conclusion is claimed from this shell.
- Generated build trees, local data, layout files, profile logs, and scratch
  experiments are ignored.

## Product Direction

The product target is an elegant, responsive, dockable desktop spectrum viewer:

- primary view: wavelength on X, flux on Y
- interaction: pan, cursor-centered wheel zoom, range navigation, spectrum
  switching, and spectral-line overlays
- windowing: freely dockable ImGui panels using the docking branch
- performance: measured on real data before making refresh-rate claims

See [docs/product_requirements.md](docs/product_requirements.md) and
[docs/technical_direction.md](docs/technical_direction.md).

## Environment

Required tools:

- Visual Studio 2022 Build Tools with the C++ desktop workload
- Windows 10/11 SDK with DirectX 11 headers and libraries
- CMake 3.24 or newer
- Ninja, if using the `ninja-msvc-debug` preset
- vcpkg

Set `VCPKG_ROOT` to your vcpkg checkout:

```powershell
[Environment]::SetEnvironmentVariable('VCPKG_ROOT', 'C:\dev\vcpkg', 'User')
```

Open a new terminal after setting it.

## Configure Check

Use the Ninja preset from a terminal with the MSVC environment loaded:

```powershell
cmd.exe /d /c "call ""C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"" && cmake --preset ninja-msvc-debug"
```

Or use the Visual Studio generator preset:

```powershell
cmake --preset vs2022-x64-debug
```

Configure success verifies the dependency stack and generated build files.

## Build

Use the Ninja preset from a terminal with the MSVC environment loaded:

```powershell
cmd.exe /d /c "call ""C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"" && cmake --build --preset ninja-msvc-debug"
```

The executable is written under `build/ninja-msvc-debug/SpecForge.exe`. Runtime
layout state is `imgui.ini`. Set `SPECFORGE_PROFILE=1` before launch to write
JSONL profile output under `logs/`.
