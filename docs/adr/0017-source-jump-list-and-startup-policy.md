# Source Jump List and startup restore policy

Status: Accepted. Date: 2026-09-20. Issue: #121.

## Startup boundary

Before constructing `SourceCollectionSession`, ordinary GUI startup chooses
`SourceSessionRestorePolicy::Restore` or `Skip`. The latter neither prepares a persisted
source restore plan nor enters its restoring lifecycle, and `ShellUi` submits
no deferred restore jobs. The explicit path then uses `OpenInitialSource` and
the existing external-source resolver and asynchronous source-opening pipeline.
Its annotation associations may be read on demand for that first source; this
does not restore other sources. After activation, live workflow state is authoritative.
A failed explicit open leaves the saved source session untouched.

Automation retains its existing restore/startup contract. Source-free startup,
including taskbar relaunch, is unchanged. This is a restore-policy seam, not a
general launch-intent framework; #68 can independently add a blank-start policy.

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
failures against the Windows COM interface. The shell activation suite verifies
construction-time restore gating, ordinary restore, first explicit activation,
stale-source errors, untouched saved state, and Files publication notifications.

The optional `spectiary_win32_jump_list_native_tests` checks real destination-list
publication using the test executable's implicit identity, then deletes that list.
`spectiary_win32_jump_list_gui_routing_tests` creates an isolated Portable copy,
proves ordinary startup forwards to a production-protocol test receiver, and
uses Windows to activate production-created `.lnk` destinations. It verifies new
ordinary GUI processes, selected-source titles and unchanged prior instances,
including a stale destination. Both are serial `real-gui;gui-integration` tests;
they clean up their processes and temporary files. Windows owns the GUI copy's
implicit identity and shell-list lifetime; the test does not attempt to discover
or delete another process's implicitly identified list.

Windows contracts: [AppUserModelIDs](https://learn.microsoft.com/en-us/windows/win32/shell/appids),
[BeginList removal semantics](https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/nf-shobjidl_core-icustomdestinationlist-beginlist),
[AppendCategory link requirements](https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/nf-shobjidl_core-icustomdestinationlist-appendcategory).
