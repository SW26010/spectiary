# SpecForge

SpecForge is being prepared as a native Windows spectrum viewer built on Win32,
DirectX 11, Dear ImGui docking, ImPlot, CMake, and vcpkg.

This repository now contains the first native shell slice for the product
direction: a Win32 + DirectX 11 executable with Dear ImGui docking, ImPlot, a
synthetic spectrum fixture, and optional JSONL profile output.

## Current State

- `specforge_native` builds the Windows executable target.
- The app shell initializes Win32, DirectX 11, Dear ImGui docking, and ImPlot.
- The main plot consumes `SpectrumSnapshotHandle` snapshots from the domain
  layer; the synthetic fixture and `.npy` loader use the same UI/plot path.
- A versioned public spectral-line catalog is loaded from
  `config/spectral_lines.public.tsv`; the Spectral Lines panel searches and
  organizes it, and the main plot renders line and band reference overlays.
- The real-data loader supports `.npy` spectrum matrices, simple
  wavelength/flux `.csv` files, and recognized single-spectrum FITS files
  focused on LAMOST/SDSS table spectra. Limited COEFF0/COEFF1 FITS image
  handling is a narrow fallback, not a generic FITS support promise.
- No real-data performance conclusion is claimed from this shell yet.
- Generated build trees, local data, layout files, profile logs, and scratch
  experiments are ignored.

## Product Direction

The product target is an elegant, responsive, dockable desktop spectrum viewer:

- primary view: wavelength on X, flux on Y
- interaction: mouse pan and cursor-centered wheel zoom, Precision Touchpad
  two-finger pan and pinch zoom, range navigation, spectrum switching, and
  spectral-line overlays
- overlay data: public rest-frame vacuum Angstrom reference markers in tracked
  config; subtype presets, zoom windows, and private criteria stay out of the
  public catalog
- windowing: freely dockable ImGui panels using the docking branch
- performance: measured on real data before making refresh-rate claims
- refresh policy: event-driven at idle; on supported Windows 11 DRR systems,
  main-plot mouse pan and Precision Touchpad manipulation request the compositor
  high-refresh clock, pace rendering from its ticks, and return to the base rate
  when interaction ends

See [docs/product_requirements.md](docs/product_requirements.md),
[docs/technical_direction.md](docs/technical_direction.md), and
[docs/spectral_line_catalog_contract.md](docs/spectral_line_catalog_contract.md).

## Environment

The built application requires Windows 10 or Windows 11 at runtime. Its
presentation path depends on `DXGI_SWAP_EFFECT_FLIP_DISCARD` and
`IDXGISwapChain3`; older Windows versions are not supported.
Windows 11 build 22000 or newer additionally enables compositor-clock DRR
boosting. When DXGI reports variable-refresh presentation support, boosted
frames use the standard tearing-capable flip-model path; ordinary and fallback
frames retain the existing display-vsync path. Windows 10 and systems without
the compositor-clock API safely keep the fallback behavior.

Required tools:

- Visual Studio 2022 Build Tools with the C++ desktop workload
- Windows 10/11 SDK with DirectX 11 headers and libraries
- CMake 3.24 or newer
- Ninja, if using the `ninja-msvc-portable-debug` preset
- vcpkg

Set `VCPKG_ROOT` to your vcpkg checkout:

```powershell
[Environment]::SetEnvironmentVariable('VCPKG_ROOT', (Join-Path $env:USERPROFILE 'vcpkg'), 'User')
```

Open a new terminal after setting it.

## Configure Check

> [!IMPORTANT]
> For Ninja/MSVC configure and build operations, use
> `scripts/build-ninja-msvc-debug.ps1`. Do not run `ninja` or
> `cmake --build --preset ninja-msvc-portable-debug` directly from an ordinary
> PowerShell or a restricted agent shell. Restricted agents must run the wrapper
> with tool escalation. See
> [Ninja/MSVC troubleshooting](docs/engineering_setup.md#ninjamsvc-卡住排查).

Use the Ninja preset from a terminal with the MSVC environment loaded:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\build-ninja-msvc-debug.ps1 -Configure
```

Or use the Visual Studio generator preset:

```powershell
cmake --preset vs2022-x64-portable-debug
```

Configure success verifies the dependency stack and generated build files.

## Build

Use the Ninja preset from a terminal with the MSVC environment loaded:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\build-ninja-msvc-debug.ps1
```

The wrapper performs a process-kill preflight and refuses to start CMake if the current
shell cannot clean up the job-assigned `cmd/cmake/ninja` process tree.

The executable is written under
`build/ninja-msvc-portable-debug/SpecForge.exe`. Portable runtime state is
owned by the executable directory's `Data` folder: ImGui layout is
`Data/specforge-imgui-v2.ini`. Release builds can start or stop a performance
diagnostic recording from `Settings > Diagnostics`, where users can also choose
the output directory. That choice is saved in `Data/profile-settings.json`.
JSONL output defaults to `Data/logs/`; resolution order is
`SPECFORGE_PROFILE_DIR` environment override, saved user setting, then the
release-profile default. `SPECFORGE_PROFILE=1` remains available for scripted
startup capture.
Recording uses a bounded asynchronous writer and stops automatically after five
minutes or 100 MiB. Stopping from Settings drains in the background; completed
recordings are accepted by the analyzer only when their final summary reports
zero dropped events.

## Portable Package

The first portable package is a no-launcher zip with `SpecForge.exe`,
`specforge_build_metadata.json`, `Data/`, and `Legal/` at the zip root.
`specforge_build_metadata.json` identifies current packages as working-tree
builds with no claimed Git revision; immutable build metadata stays outside the
mutable `Data/` user-state directory. `Legal/` contains the SpecForge EULA,
complete third-party software notices, and scientific data attribution:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\build-portable.ps1
```

The script invokes CMake directly with the Visual Studio portable release
preset, so run it from a normal developer shell or an approved unsandboxed agent
run.

The package is staged under `dist\SpecForge-portable` and zipped as
`dist\SpecForge-portable.zip` with a matching `.sha256` file.

Launch with a spectrum source path to smoke-test the real-data loader:

```powershell
.\build\ninja-msvc-portable-debug\SpecForge.exe C:\path\to\spectrum_source.fits
```

Inside the app, the Files panel `Add file...` button opens source files through
the same domain snapshot loader, and `Add folder...` opens the native Windows
folder picker to add a directory source to the session list. Folder loading is
non-recursive and treats first-level `.csv` and FITS files as one navigable
collection. Subfolders, unrelated file types, and mixed CSV/FITS folders are
reported as diagnostics. The file picker exposes common candidate source
suffixes such as `.npy`, `.csv`, and FITS variants. Unsupported files and
catalog FITS files stay visible in the Files panel as domain-produced error
snapshots with diagnostics.

A 3909-column `.npy` matrix uses the fixed loglam wavelength grid; other column
counts fall back to pixel index and report that in snapshot diagnostics. Known
auxiliary arrays such as `*_label.npy`, `*_index.npy`, `*_ormask.npy`, and
`*_known_mask.npy` are rejected instead of being plotted as spectra.

On a Windows Precision Touchpad, move the pointer over the spectrum and use two
fingers to pan or pinch to zoom. Starting over the plot body controls both axes;
starting over an axis region constrains the gesture to that axis. The docked
plot, immersive plot, and detached viewport share the same behavior. Devices
without native Precision Touchpad input retain the existing mouse and wheel
controls.

## Interaction Profiling

For the standard ImPlot pan/drag responsiveness flow, see
[docs/performance_testing.md](docs/performance_testing.md). For implementation
constraints and regression case studies, see
[docs/ui_responsiveness.md](docs/ui_responsiveness.md). The short path is:

```powershell
powershell -ExecutionPolicy Bypass -File scripts\profile-implot-pan.ps1 -InitialSource "C:\path\to\source.npy"
```
