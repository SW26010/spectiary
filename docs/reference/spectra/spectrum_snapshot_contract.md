# Spectrum Snapshot Contract

## Purpose

`domain` owns the real-data input contract. UI and plot code consume one stable
snapshot at a time; they do not parse FITS/NPY, do not know loader types, and do
not depend on whether the snapshot was produced by native C++, Python, IPC, or an
external preprocessing step.

Opening a new source, changing the current spectrum index, or applying a domain
operation that changes the current spectrum data creates a new snapshot. Existing
snapshots are not mutated in place, so rendering code can safely keep reading
the current state for the duration of a frame.

The C++ interface is `src/domain/spectrum_snapshot.h`.
Snapshot producers should hand snapshots to consumers as `SpectrumSnapshotHandle`
(`std::shared_ptr<const SpectrumSnapshot>`), not as mutable UI state.

## Snapshot Shape

`SpectrumSnapshot` has six top-level sections:

- `source`: stable source identifier, display name, path or URI, and source-level
  metadata.
- `collection`: total spectrum count, current index, and previous/next
  switchability.
- `current_spectrum`: spectrum display name, x values, y values, point count,
  and spectrum-level metadata.
- `axis`: x/y semantic type, unit, and coordinate frame.
- `capabilities`: domain-derived feature flags that let UI decide what controls,
  overlays, and warnings are valid.
- `diagnostics`: domain-level reasons for failure, unsupported format, empty
  data, invalid shape, and mask/ivar pixel rejection.

## Invariants

A loaded, plottable snapshot must satisfy:

- `collection.spectrum_count > 0`.
- `collection.current_index < collection.spectrum_count`.
- `current_spectrum.x_values` and `current_spectrum.y_values` are non-null.
- `current_spectrum.x_values->size() == current_spectrum.y_values->size()`.
- `current_spectrum.point_count` equals that shared vector size.
- Plot-visible x/y values are finite.
- Wavelength x values are positive and sorted ascending.

An error snapshot must satisfy:

- `collection.spectrum_count == 0`, unless the source was opened and only the
  selected row failed.
- `capabilities.can_plot_current_spectrum == false`.
- `diagnostics` includes at least one `Error` diagnostic explaining the domain
  reason.

Pixel-rejection diagnostics are allowed on otherwise plottable snapshots. For
example, mask or ivar validity rules can reject invalid pixels and still produce
a valid current spectrum, but the snapshot should record that rejection happened.

## Axis And Capabilities

Axis semantics are explicit:

- `x.quantity`: `Wavelength`, `Pixel`, or `Unknown`.
- `x.unit`: `Angstrom`, `Pixel`, or `Unknown`.
- `x.frame`: `Rest`, `Observed`, or `Unknown`.

Plot-visible axis labels use the normalized snapshot semantics: Angstrom
wavelength is `Wavelength (Å)`, a generated sample-position axis is
`Pixel Index`, explicitly identified flux is `Flux`, and other numeric values
use the neutral label `Value`. A loader must not claim normalized flux from
numeric values alone.

Capabilities are read-only UI guidance derived by domain code from axis,
metadata, and diagnostics. UI should not re-derive these rules itself.

Important flags:

- `can_plot_current_spectrum`: main plot may render x/y.
- `can_show_spectral_lines`: spectral-line overlay is meaningful for this x
  axis.
- `can_show_rest_frame_spectral_lines`: rest-frame line overlays are valid
  without warning.
- `requires_angstrom_warning`: the x axis is not confirmed to be Angstrom.
- `requires_rest_frame_warning`: rest-frame overlays would be ambiguous or
  potentially misleading.

`Observed` wavelength axes should not silently display rest-frame standard line
tables. `Unknown` axes may show reference overlays only with an explicit warning.
The concrete public line-table schema and UI/plot overlay rules are documented
in the [Spectral Line Catalog Contract](../spectral-lines/spectral_line_catalog_contract.md).

## Display-Only Plot Transforms

Plot smoothing is view state, not sample filtering or any other domain-level
sample subset operation. It must not mutate
`current_spectrum.x_values`, `current_spectrum.y_values`, diagnostics, or source
metadata, and it must not be serialized as part of a `SpectrumSnapshot`.
In Spectiary terminology, this word family is reserved for qualified
sample-filter language; see [CONTEXT.md](../../../CONTEXT.md).

Gaussian smoothing uses sigma in loaded sample-index units. A sigma value of `2`
means two adjacent plotted samples, regardless of the x-axis unit or spacing; it
is not measured in wavelength, Angstrom, pixel coordinate, or any other physical
x-axis unit. If Spectiary later needs wavelength-width smoothing, that should be
added as a separate mode with explicit axis-unit semantics.

## Source Boundaries

The snapshot contract deliberately excludes loader mechanics:

- No FITS HDU rules in UI or plot code.
- No NPY shape or mmap rules in UI or plot code.
- No native/Python/IPC distinction in UI or plot code.
- No catalog parsing in UI or plot code.

Format-specific rules stay in the producer side of `domain`. The producer may
use [data formats](data_formats.md) as input policy, but it only exposes the normalized
snapshot.

UI may keep a session-level source list and may expose broad file picker or
folder path-entry affordances for likely spectrum sources. Those affordances are
not loader decisions or support claims. Every selected source must still go
through a domain snapshot producer, including unsupported files and folders.

Unsupported sources, including folders with no supported spectra, are represented as domain-created error snapshots. UI may
display their normalized `source` metadata, `capabilities`, and `diagnostics`,
but must not infer parser support, synthesize placeholder snapshots, or
recompute source-type diagnostics itself.

## Current Fixture

The synthetic fixture may continue to exist for shell testing. It must be marked
as synthetic through source metadata and should not be used for real-data
performance claims. The current `.npy` loader already produces the same
`SpectrumSnapshot` shape for 1D arrays and row-level 2D arrays; future loaders
must keep that UI/plot boundary intact.
