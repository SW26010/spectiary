# SpecForge

SpecForge is being prepared as a native Windows spectrum viewer built on Win32,
DirectX 11, Dear ImGui docking, ImPlot, CMake, and vcpkg.

This repository is currently in the first-commit cleanup phase. The goal of this
phase is to make the project direction, environment, and documentation clean
before product code is added.

## Current State

- Product code is intentionally not implemented yet.
- The retired GLFW/OpenGL demo has been moved out of the commit path.
- `CMakeLists.txt` validates the declared dependency stack but does not declare
  an executable target.
- Generated build trees, local data, layout files, and scratch experiments are
  ignored.

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

At this phase, configure success is the expected verification. There is no
application executable to run yet.
