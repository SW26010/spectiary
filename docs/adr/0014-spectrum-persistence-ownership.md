# Spectrum preference and viewport ownership (#111)

Status: Accepted. Date: 2026-09-19.

`SpectrumPlotPreferences` owns only `SpectrumPlotColors`. Its independent codec
reads/writes `config/spectrum-plot-preferences.json` beneath the resolved
`application_data_root`. `SpectrumViewportState` owns lock state, source
collection identity and x/y limits in `state/spectrum-viewport-state.json`.
An explicit runtime/automation root override redirects both owners in either
storage profile, including Portable. Package-relative locators continue to use
the executable-based `package_root`, independently of this isolated data root.
Both use schema 1, with format kinds `spectiary.spectrum_plot.preferences` and
`spectiary.spectrum_viewport.state`. The historical `specforge.*` kinds are
explicit bounded read inputs only; all current writes use the Spectiary kinds
under [ADR 0011](0011-project-identity-contracts.md). The ownership split does
not change source identity or rendering.

ShellUi retains a lifecycle and startup writeback authority for each owner.
Color changes dirty only preferences. Viewport mutation revision changes dirty
only viewport state; persisted snapshot comparison additionally observes source
identity changes and fitted/restored limits. Render and maintenance observe
changes, and final flush observes the last snapshot before flushing only pending
writes. Loading defaults or shutting down does not itself dirty either owner.
Warnings, retry status and final save failures identify the individual owner.

Missing files permit later writes. Whole-document read/parse/version failures
retain runtime fallback and diagnostics but prohibit that owner's automatic
writeback, following ADR 0013. Supported field-level recovery remains writable:
invalid colors fall back per curve, and invalid locked limits use automatic
range. A color reset to Auto is a preference mutation; it does not authorize
overwriting an unsupported document. Viewport loss/reset does not reset colors.
This change adds no global reset UI or generic preferences manager.

The startup viewport snapshot remains pending until deferred source restoration
resolves. Only a matching identity and an unchanged user viewport revision may
restore its limits. Closing before restoration finishes retains the pending
snapshot; changing colors does not erase it.

## Bounded migration

`legacy_spectrum_view_state_path` is read-only migration input at the prior
transitional location. The old combined save API has been removed. Migration
accepts old schema 1/2, maps colors to preferences and lock/identity/limits to
viewport state, and fills only missing destinations. Existing destinations,
including invalid or unsupported ones, remain authoritative. When neither
destination is missing, the legacy document is not decoded.

A failed legacy load neither overwrites/deletes that input nor authorizes
fallback writes into missing destinations. An existing healthy destination
remains independently writable. As tightened by the final #103 cutover
([ADR 0015](0015-application-storage-cutover.md)), a failed target import retains
the destination/defaults and diagnostics, keeps the legacy input, and does not
schedule ordinary autosave of legacy values. This prevents a delayed migration
retry from overwriting a new owner created by another instance. Imports publish
complete staged files without replacing concurrently created destinations.
The next startup can complete a partial migration without rewriting an already
established target. Only after both destinations are healthy and established is
the exact old regular file removed; cleanup failure leaves split files authoritative.
There is no cross-file transaction or live merge. Ordinary explicit mutations
retain their independent complete-snapshot save policies.

ADR 0015 subsequently cuts over the remaining application-managed owners.
This spectrum split does not change their domain ownership or lifecycle.

Regression coverage checks final role paths across deployment profiles, separate
codecs, dirty triggers, byte/timestamp preservation of the untouched owner,
state loss and color reset, independent failure authority and diagnostics,
legacy schema recovery, partial migration/restart and failed migration safety,
plus existing deferred viewport restore and rendering tests.
