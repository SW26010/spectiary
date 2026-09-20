# Source Jump List and startup restore policy

Status: Accepted. Date: 2026-09-20. Issue: #121.

## Startup boundary

All ordinary GUI startups restore the persisted Files roster. First source-free
startup restores saved activation; explicit-source startup overrides that target.
`OpenInitialSource` runs before the message loop drains source completions and
uses the existing external-source resolver and asynchronous opening pipeline.
Its explicit path overrides the deferred transaction's desired active source.
Other saved sources continue restoring into Files, but are not presented before
the selected source. Thus a saved roster [A, B] with B active becomes [A, B]
with A active when A is selected from the Jump List; publication retains B.
A successful explicit activation marks the source session for persistence after
restore, so the full roster and overridden active source survive a restart.
A stale explicit path reports ordinary opening diagnostics without presenting
another restored source as a fallback or changing an existing running instance.
If that path belongs to an unresolved saved source, it retains a removable
failure row in Files even when the explicit open replaced its restore job.
An invalid path that was not saved does not add a Files row.

For #68, before constructing the application, a subsequent ordinary source-free
launch queries ADR 0016's existing namespace-local, protocol-compatible endpoint
discovery. A registered peer selects
`SourceSessionStartupPolicy::RestoreRosterWithoutActive`: restore all Files rows
without a startup activation target or current sample. Prepared background sources
register their roster entries and navigation contexts without activating the plot
or labeling workflow. An idle flush or close does not replace the saved active
source with null. Explicit source mutations then use normal persistence, including
selections and removals while background restore is still pending.
Application preferences and layout retain their ordinary storage lifecycle.
Explicit-source startup uses `RestoreSavedActive` with its explicit override.
Automation and resource workloads are
excluded. Discovery neither activates a peer nor sends a source request.
Endpoint registration remains at ordinary GUI initialization; simultaneous
launches before either endpoint is registered can both restore. No input polling,
shell-origin detection, startup mutex or additional presence registry is added.
There is no generic launch-intent framework or separate shell source loader.

## Windows shell identity

The publisher runs inside the GUI process and uses Windows' implicitly assigned
application identity. It does not call `SetAppID`, set a process/window AppID,
or attach an AppID or relaunch properties to source links or windows.
Config namespaces do not impose separate taskbar groups. If Windows groups
multiple deployments together, the last publisher replaces their shared Jump
List with its current Files projection; there is no cross-deployment roster merger.
Explicit identity and shortcut/installer alignment are deferred to #118, where
that user-visible application grouping policy can be decided together.
Automation and resource-workload fixtures do not publish user Jump Lists.

## Projection and user removals

`source_roster_changed` marks a snapshot for publication after pending source
loads settle. Ordinary startup, foreground reactivation and language changes
also refresh it. Files remains the only source roster. Entries keep Files order,
must have an absolute existing regular-file or directory path, and use the
current executable with `--new-instance <quoted source>`; ADR 0016's routing gate
therefore cannot forward them. Missing paths are omitted on refresh. Paths that
become stale after publication fail through ordinary source-opening diagnostics.
Windows controls displayed item counts; the publisher respects its slot limit.

A single background STA worker performs filesystem and shell I/O, coalescing
pending updates to the latest snapshot. A fixed publication mutex serializes the
shell transaction and removal-preference updates across Spectiary processes.
This lock does not assign an application identity or control taskbar grouping.
Failed COM transactions abort and report through debugger diagnostics without changing
application source state. Windows privacy restrictions may suppress the category.

Shutdown discards pending snapshots and requests cancellation. The worker owns
its paths, snapshots and COM apartment independently of the GUI; teardown does
not join an in-flight filesystem or shell call. Such calls are not forcibly
interrupted: when they return, cancellation checkpoints stop further publication
and abort any uncommitted list. An already-entered commit may finish. Process
exit can end outstanding work; Jump List refresh is best-effort, not a shutdown
durability requirement.

Windows clears its removed-destinations list after a successful `CommitList`.
Consequently `config/shell-jump-list-exclusions.txt` retains only SHA-256 source
identity digests as negative shell preferences. Its first line is
`spectiary-jump-list-exclusions-v1`, followed by one lowercase 64-digit hex digest
per line. It contains no path, title, ordering or active-source state, and cannot
supply a candidate source. Preferences are merged and atomically saved before
Windows can clear its removal records. Read/write failure preserves those records
by aborting publication. These exclusions survive restarts and are namespace-local;
closing that namespace's instances and deleting this preference file resets them.
They do not provide cross-configuration removal synchronization if Windows shares
one implicit shell identity across those configurations.

## Verification

`spectiary_win32_jump_list_tests` covers native Shell Link content, eligibility,
ordering, limits, removal persistence, configuration-local preferences and transaction
failures against the Windows COM interface.
Blocked-publication tests also verify nonblocking teardown, discarded queued
snapshots, owned worker lifetime and cancellation without committing a replacement.
The shell activation suite verifies
ordinary roster/saved-active restore, explicit activation overriding a saved
active source while retaining the other sources, out-of-order load completion
without presenting an unrelated source, full-roster Jump List projection, stale
source diagnostics, and roster restoration without startup activation.
Router tests also cover read-only peer discovery, protocol compatibility,
namespace isolation and process exit. The GUI routing smoke checks a first
source-free restore followed by a distinct no-active GUI and non-destructive close.

The optional `spectiary_win32_jump_list_native_tests` checks real destination-list
publication using the test executable's implicit identity, then deletes that list.
`spectiary_win32_jump_list_gui_routing_tests` creates an isolated Portable copy,
proves ordinary startup forwards to a production-protocol test receiver, and
uses Windows to activate production-created `.lnk` destinations. It verifies new
ordinary GUI processes, selected-source titles and unchanged prior instances,
including a stale destination, and verifies that the child persists both sources.
Both are serial `real-gui;gui-integration` tests;
they clean up their processes and temporary files. Windows owns the GUI copy's
implicit identity and shell-list lifetime; the test does not attempt to discover
or delete another process's implicitly identified list.

Windows contracts: [AppUserModelIDs](https://learn.microsoft.com/en-us/windows/win32/shell/appids),
[BeginList removal semantics](https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/nf-shobjidl_core-icustomdestinationlist-beginlist),
[AppendCategory link requirements](https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/nf-shobjidl_core-icustomdestinationlist-appendcategory).
