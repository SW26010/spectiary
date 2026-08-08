# Detached Panels Use Independent Viewports with Win32 Ownership

Status: Accepted.

## Context

SpecForge uses Win32, DirectX 11, Dear ImGui docking/multi-viewport, and a
modern `FLIP_DISCARD` presentation path. Floating product panels need to remain
responsive, independently renderable, and stable while they are moved across
the main-window boundary, minimized, restored, focused, docked, or undocked.

The window model converged through three related regressions:

- [#35](https://github.com/SW26010/SpecForge/issues/35) showed that
  `FLIP_DISCARD` combined with Dear ImGui viewport AutoMerge can expose a
  cross-HWND presentation handoff gap while a floating panel repeatedly merges
  into and separates from the main viewport. Keeping floating panels in stable
  secondary viewports with `ConfigViewportsNoAutoMerge = true` avoided that
  flicker without abandoning the modern presentation path.
- [#48](https://github.com/SW26010/SpecForge/issues/48) then showed that stable
  secondary viewports implemented as unrelated top-level HWNDs do not naturally
  follow the main window's minimize/restore lifecycle and can become difficult
  to reach. SpecForge temporarily synchronized secondary visibility explicitly.
- [#53](https://github.com/SW26010/SpecForge/issues/53) showed the remaining
  architectural gap: explicit hide/show does not create a persistent Win32
  window group, so another application's window can interleave between the main
  HWND and detached panels after restore.

The required distinction is therefore between *viewport independence* and
*operating-system window ownership*. A detached panel should keep its own ImGui
viewport, native HWND, swap chain, and presentation identity, while still being
recognized by Windows as an auxiliary window belonging to the SpecForge main
window.

## Decision

SpecForge standardizes the detached-panel window stack as:

- keep the current `FLIP_DISCARD` presentation architecture;
- keep `io.ConfigViewportsNoAutoMerge = true`;
- set `io.ConfigViewportsNoDefaultParent = false`;
- let the Dear ImGui Win32 backend translate the main viewport relationship into
  the native Win32 owner/owned-window relationship for secondary platform
  windows.

A detached panel is consequently independent for rendering and presentation but
owned for native window management:

```text
ImGui:   detached panel -> independent viewport
DXGI:    detached panel -> independent swap chain / presentation target
Win32:   detached panel -> top-level window owned by the main HWND
```

This is an owner relationship, not a `WS_CHILD` child-window relationship.
SpecForge must not replace it with `SetParent()` or otherwise introduce child
coordinates/clipping semantics.

Windows owns the normal minimize/restore and z-order grouping behavior of these
owned windows. SpecForge must not reintroduce a general secondary-window
hide/show list, a restore-time `SetWindowPos`/`DeferWindowPos` z-order manager,
or `WS_EX_TOPMOST`/`HWND_TOPMOST` as a substitute for ownership. Explicit focus
requests remain appropriate when the user performs an action whose meaning is
"show/reactivate this already-open panel"; that is a user focus intent, not
window-group maintenance.

## Consequences

The following behavior is intentional and accepted:

- A detached panel remains above its SpecForge owner window. Clicking the main
  window does not place the main window above an owned detached panel.
- Detached panels are **not** globally topmost. A normal window from another
  application can cover the SpecForge main window and its detached panels.
- Minimizing the main window hides its owned detached panels without requiring
  application-maintained visibility bookkeeping; restoring the owner restores
  the group while preserving the secondary HWND identities.
- `NoAutoMerge` means an undocked panel remains a distinct platform viewport
  even when positioned wholly inside the main-window client area. Explicit
  docking is what returns the panel to a dock host.
- Each visible detached viewport retains its own native window and presentation
  resources, so additional floating panels can increase HWND, swap-chain,
  rendering, and Present work. This cost is part of the chosen stable-viewport
  model and must be measured rather than hidden by changing ownership semantics.

The "owned panel always above its main window" behavior is a deliberate UX
tradeoff. It is preferable to making panels peer top-level windows and then
reimplementing application grouping, lifecycle, activation, and z-order in
SpecForge. A future product requirement that the main window must be able to
cover detached panels would conflict with this decision and requires an explicit
architecture review rather than an ad hoc owner toggle.

## Rejected Alternatives

### `FLIP_DISCARD` with viewport AutoMerge

Rejected for the current renderer because #35 demonstrated visible viewport
handoff flicker during merge/separate transitions. Re-enabling AutoMerge must
first demonstrate that the relevant presentation/composition behavior has
changed and that the #35 regression no longer occurs.

### `NoAutoMerge` with unowned top-level secondary HWNDs

Rejected because #48 and #53 demonstrated the resulting lifecycle and global
z-order gaps. Independent viewports do not require unrelated Win32 top-level
windows.

### Global topmost detached panels

Rejected because `WS_EX_TOPMOST`/`HWND_TOPMOST` changes cross-application
semantics. SpecForge tool windows must not remain above unrelated foreground
applications merely to stay above their own main window.

### Application-managed hide/restore and z-order reconstruction

Rejected because explicit `ShowWindow()` and restore-time ordering only repair a
particular transition; they do not establish a durable native ownership
relationship. Maintaining the same policy in application code would duplicate
Win32 window-group semantics and create a larger activation/z-order state
machine.

### Dynamically attaching and detaching owners

Rejected because toggling ownership to let the main window sometimes cover a
panel would make task switching, activation, minimize/restore, and z-order
behavior state-dependent. The additional complexity is not justified by the
current floating-tool-window product model.

## Validation Contract

The architecture check must continue to protect both policy assignments:

```cpp
io.ConfigViewportsNoAutoMerge = true;
io.ConfigViewportsNoDefaultParent = false;
```

The interactive viewport ownership regression must continue to validate at
least:

- multiple detached viewport HWNDs retain their identities while moved inside
  and outside the main client area;
- each secondary HWND reports the main HWND as its Win32 owner;
- detached HWNDs do not carry `WS_EX_TOPMOST`;
- minimizing the main HWND hides owned secondary HWNDs without destroying them;
- after an independent cross-process peer window becomes foreground, restoring
  and activating SpecForge restores the same owned HWNDs;
- the visible SpecForge owner group has a coherent non-topmost z-order and is
  not interleaved by that peer during the tested restore path.

The existing interactive check is:

```powershell
ctest --test-dir build\ninja-msvc-debug -C Debug -L gui-integration --output-on-failure
```

Presentation-specific changes must additionally preserve the #35/#36 behavior
on a real interactive Windows desktop; framebuffer-only tests cannot prove the
absence of a cross-HWND DWM composition flicker.

## Related History

- #35 — AutoMerge + `FLIP_DISCARD` viewport handoff flicker.
- #36 — floating viewport interaction/presentation cadence.
- #48 — detached-panel minimize/restore and reactivation gaps.
- #53 — restore-time application-window-group z-order gap.
- `170a2fef14f70bb5969ec4d4e904ebebf1bc5e5a` — keeps floating panels in
  independent viewports.
- `3f2a24f2252a7504e6490bb30d3a065096eaaf39` — temporary explicit detached
  window lifecycle synchronization.
- `e89e526c4805623cc867e8a58a1baa5130bf26ac` — establishes the current
  Win32 owner-group implementation and regression coverage.
