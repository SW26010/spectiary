<p align="center">
  <img src="resources/branding/spectiary.svg" width="128" alt="Spectiary logo">
</p>

<h1 align="center">Spectiary</h1>

Formerly SpecForge. See [project identity and rename contracts](docs/project_rename.md)
for the pre-1.0 format cutover and historical compatibility boundaries.

<p align="center">
  <a href="https://github.com/SW26010/spectiary/actions/workflows/automation.yml"><img src="https://github.com/SW26010/spectiary/actions/workflows/automation.yml/badge.svg?branch=master&amp;event=workflow_dispatch" alt="Spectiary automation status"></a>
  <a href="https://github.com/SW26010/spectiary/releases/latest"><img src="https://img.shields.io/github/v/release/SW26010/spectiary?label=Release&amp;color=2ea44f&amp;logo=github&amp;logoColor=white" alt="Latest Spectiary release"></a>
  <img src="https://img.shields.io/badge/C%2B%2B-20-00599C?logo=cplusplus" alt="C++20">
  <img src="https://img.shields.io/badge/Dear%20ImGui-Docking-4B8BBE" alt="Dear ImGui with docking">
  <img src="https://img.shields.io/badge/ImPlot-Plotting-8A2BE2" alt="ImPlot">
  <img src="https://img.shields.io/badge/Platform-Windows%2010%20%7C%2011-0078D4?logo=windows11" alt="Windows 10 and 11">
</p>

<p align="center">
  <strong>Lightweight. Fast. Fluid.</strong><br>
  A native Windows spectrum viewer for fast, focused inspection of LAMOST and SDSS spectra.
</p>

Spectiary is built for the part of spectral analysis that happens with your eyes and hands: opening local astronomical spectra, moving rapidly through observations, zooming into features, and comparing them with spectral references without carrying a heavy application stack along for the ride.

It is designed as a focused desktop tool rather than a general scientific platform. The main plot stays at the center of the experience, interaction latency is treated as a product requirement, and features are expected to preserve the responsiveness of pan, zoom, navigation, and spectrum switching.

> [!NOTE]
> Spectiary is currently **pre-1.0 and under active development**. The core viewing workflow is usable today, while FITS coverage and higher-level analysis workflows are still evolving.

## Why Spectiary

### Lightweight

Spectiary is a native **C++20** Windows application built directly on Win32 and DirectX 11. It does not require a browser runtime or a Python runtime to inspect spectra, and it avoids a large cross-platform UI framework in the hot path.

The application also uses an event-driven render policy: when nothing is changing, it does not keep redrawing just to look alive.

### Fast

The architecture keeps data loading, diagnostics, and other potentially expensive work away from the plot interaction path. Spectrum sources can load in the background, while the UI consumes stable spectrum snapshots rather than parsing files inside the renderer.

Performance work is validated with **real spectral data and recorded interaction timing**, not synthetic-only benchmark claims.

### Fluid

The main plot is designed around direct manipulation:

- drag to pan
- cursor-centered wheel zoom
- Windows Precision Touchpad two-finger pan and pinch zoom
- axis-constrained touchpad gestures when starting over an axis
- wavelength and flux range navigation
- fast previous/next spectrum switching for supported multi-spectrum sources

On supported Windows 11 systems, Spectiary can integrate with **Dynamic Refresh Rate (DRR)** through the Windows compositor clock so active plot interaction can request a higher-refresh presentation path and return to the base rate afterwards.

## What You Can Do

- **Open LAMOST and SDSS FITS spectra** directly in a native desktop viewer.
- **Move rapidly through supported spectrum collections** and multi-spectrum FITS sources.
- **Open folders of spectra** for lightweight local review workflows.
- **Pan, zoom, and inspect features** with mouse or Precision Touchpad input.
- **Navigate wavelength and flux ranges** without losing the main plot as the primary workspace.
- **Search spectral references** and display line and band overlays directly on the plot.
- **Arrange the workspace freely** with dockable and detachable panels.
- **Enter an immersive plot view** with `F11` when the spectrum itself needs the full screen.
- **Record bounded performance diagnostics** when investigating interaction or presentation behavior.

## Designed For

Spectiary is especially suited to workflows such as:

- visually inspecting **LAMOST and SDSS FITS spectra** on a local Windows workstation
- rapidly reviewing many observations for quality control, candidate triage, or manual inspection
- zooming into local wavelength regions and comparing features with reference lines or bands
- moving repeatedly between neighboring spectra without breaking visual focus
- long desktop inspection sessions where low interaction latency and high information density matter
- high-refresh Windows desktops and laptops where the plotting surface should feel as direct as the rest of the system

## FITS First

Spectiary's user-facing data path is centered on astronomical FITS spectra, with **LAMOST and SDSS as the primary compatibility targets**.

| Source | Current support |
| --- | --- |
| LAMOST FITS | Recognized single-spectrum table/image semantics and supported vector-table cases |
| SDSS FITS | Recognized SDSS-style table/image semantics and supported vector-table cases |
| Generic FITS | Restricted single-spectrum table/image semantics with explicit wavelength metadata |
| Folder of spectra | Non-recursive browsing of supported first-level FITS sources |

FITS is a broad ecosystem, so support is intentionally explicit rather than claiming that every FITS layout will work. A single CFITSIO reader parses both table and image containers; the spectrum semantics layer then recognizes only the restricted LAMOST, SDSS, and generic single-spectrum structures described by the format contract. Opening a container is not itself a promise that Spectiary can interpret it as a spectrum.

For `.fits.gz`, Spectiary first performs bounded transport decompression and then gives the resulting FITS bytes to that same CFITSIO reader. FITS network URLs and FITS writing are not supported.

Spectiary also has `.npy` and simple wavelength/flux `.csv` input paths for project-specific datasets, development, testing, and conversion workflows. They are useful implementation contracts, but they are **not the formats that define the public-facing product**.

For the exact loader and data-boundary behavior, see [Spectrum Snapshot Contract](docs/spectrum_snapshot_contract.md).

## A Native, Performance-First Stack

Spectiary deliberately uses a small native stack:

- **C++20** for the application and domain layer
- **Win32** for native Windows integration
- **DirectX 11 / DXGI flip model** for presentation
- **Dear ImGui docking branch** for a flexible desktop workspace
- **ImPlot** for interactive spectrum plotting
- **Windows Direct Manipulation** for native Precision Touchpad gestures
- **Windows 11 compositor clock / DRR integration** for high-refresh interaction where supported
- **CMake + vcpkg** for reproducible project configuration and dependency management

This stack is not an abstraction exercise. It is chosen to keep the path from input to plot update to presentation short, observable, and maintainable.

## Workspace and Interaction

The main spectrum plot is the first visual layer. Supporting tools live in ordinary dockable panels, so the workspace can be rearranged without replacing the plotting surface with a custom window-management system.

Panels can be docked, undocked, and restored through the Dear ImGui docking layout. Detached panels use native Windows viewports, while `F11` provides a dedicated immersive plot presentation for focused inspection.

Spectral references come from the tracked public catalog in [`config/spectral_lines.public.tsv`](config/spectral_lines.public.tsv). The Spectral Lines panel can search and organize those references and render line or band overlays on the main plot.

## Platform

- **Windows 10 or Windows 11**
- **64-bit Windows on x64 hardware**
- Official releases are provided for x64 only
- Windows 11 adds optional compositor-clock DRR boosting on supported systems
- Windows Precision Touchpad gestures use the native Windows Direct Manipulation path

The presentation path uses modern DXGI flip-model behavior and does not target Windows versions older than Windows 10.

## Development

README is intentionally kept user-facing. Build configuration, implementation contracts, profiling details, automation interfaces, and packaging rules live in the documentation instead.

Start here if you want to build or work on Spectiary:

- [Engineering setup](docs/engineering_setup.md) — toolchain, vcpkg, CMake presets, build and test guidance
- [Technical direction](docs/technical_direction.md) — architecture and performance constraints
- [Product requirements](docs/product_requirements.md) — product goals, workflows, milestones, and non-goals
- [Performance testing](docs/performance_testing.md) — real-data interaction profiling
- [Release artifacts](docs/release_artifacts.md) — portable packaging, metadata, hashes, and release contracts
- [Automation control](docs/automation_control.md) — test/debug automation interface
- [Spectral line catalog contract](docs/spectral_line_catalog_contract.md) — public reference data rules

The current native stack requires Visual Studio 2022 Build Tools, a Windows 10/11 SDK, CMake 3.24 or newer, and vcpkg. Ninja is optional. See [Engineering setup](docs/engineering_setup.md) for the supported commands rather than invoking the Ninja/MSVC build path ad hoc.

## License

Spectiary has not yet specified a license for its own source code. The planned license when the project is open-sourced is MIT; that plan is not currently in effect.

[Third-party software notices](legal/THIRD_PARTY_NOTICES.txt) and [data source notices](legal/DATA_SOURCES.txt) cover their respective components and data, and do not specify a license for Spectiary itself.

---

<p align="center">
  <strong>Spectiary is about one thing first: making spectrum inspection feel light, fast, and fluid.</strong>
</p>
