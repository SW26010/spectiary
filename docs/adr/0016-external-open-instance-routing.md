# External source opens route within a runtime namespace

Status: Accepted. Date: 2026-09-20. Issue: #4.

## Scope and ownership

The invocation resolves its existing startup storage context and reads the
external-source settings from that context's `config/`. `instance_policy` is
`new_instance` (also the missing/upgrade default) or `recent_instance`. Settings
remain complete startup snapshots under ADR 0004; the receiver does not reread
or synchronize its GUI settings when a request arrives.

Only ordinary GUI startup with a source argument may forward. Explicit
`--new-instance`, automation, resource-workload fixtures, and launches without a
source bypass routing. Files' **Open in a new instance** passes the explicit
flag through the existing current-executable launcher. Forwarding failure
continues the current invocation's normal startup, without recursive spawning.

The discovery scope is the normalized runtime `config_root`, hashed to a Win32
message-only window class under the immutable founding-identity prefix (ADR 0011).
Windows session/desktop isolation also applies.
Portable roots and Installed/Standalone roots that differ cannot see one another.
Executables resolving the same namespace may interoperate only when their wire
protocol versions match. There is no process enumeration, machine-wide registry,
EXE/version compatibility matrix, persistent instance history or primary process.

## Transient protocol and lifetime

Each ready ordinary GUI registers a message-only endpoint on its GUI thread.
Protocol and last-activation performance-counter stamps are transient window
properties. App activation updates recency, including activation of detached
windows in the process. Endpoint destruction and process exit remove registration.
Normal close withdraws the endpoint after the unsaved-work close check succeeds.
Automation named pipes are not involved.

Discovery considers up to 256 namespace endpoints and selects the most recently
used compatible endpoint. Discovery, transfer and activation share a 1500 ms
budget. No target, protocol mismatch, stale handle, closing target, hung receiver,
activation refusal or an unaccepted timeout continues normal new-instance startup.
An overfull receive batch (64 requests) also declines ownership.

The sender gives foreground permission to the target process. The GUI receiver
restores a minimized main window and requests foreground activation using
documented Windows APIs. Focus failure does not open the source in the receiver.
Windows foreground restrictions are respected; no simulated input or thread-input
attachment is used to override them.

`WM_COPYDATA` carries the name of a unique session-local shared-memory request.
The mapping contains a fixed-width protocol header, absolute UTF-16 path, folder
interpretation flag, deadline, and an interlocked state. Paths are bounded to
32767 UTF-16 code units. The namespace prefix and protocol are checked again by
the receiver. No source filesystem probing occurs in IPC handling.

The receiver first prepares its owned queue entry and activates the window, then
atomically changes pending to accepted. On timeout the sender changes pending to
canceled; exactly one side wins. A canceled/expired request cannot later open in
the receiver. An accepted request suppresses fallback even if its reply times
out. This resolves the ambiguous acknowledgement race without durable journaling.
Acceptance transfers responsibility to the running GUI, not durable delivery
across a later crash or explicit user close.

## Source semantics

The request carries the invocation's folder preference and original absolute
source path. A new source uses the production external-open resolver, preserving
the preferred FITS/CSV member. It never degrades to a directory-only request.
Routed opens are serialized through the existing load transaction so a later
activation cannot cancel an earlier accepted source load. Repeated requests
during loading resolve against the completed roster.

Existing sources are activated without reload, preserving their selected sample
and workflow. A folder-member request explicitly navigates to that member in
the existing collection. Since #117, the activation transaction shares this
member resolution with in-app opens and automation `source.open`, preferring the
active containing source and then roster order. Explicit member display keeps
sample filtering and sorting intact; an excluded displayed row has no sequence
cursor. Missing/new or stale members use ordinary open resolution. Source
loading and navigation remain asynchronous; new-process parent-folder
interpretation remains independent.

## Verification

`win32_external_open_router_tests` uses real child processes, message-only windows
and mappings to cover namespace isolation, recency, incompatible versions, exit,
hang, focus refusal, cancellation racing delayed focus, and accepted requests
whose reply times out. It also tests native minimized-window restoration and
foreground activation. Shell tests cover duplicate snapshot preservation,
queued requests, multiple sources and non-first preferred members. Settings and
widget tests cover default, persistence, invalid values, failed saves and controls.

Platform references: [message-only windows](https://learn.microsoft.com/en-us/windows/win32/winmsg/window-features),
[SendMessageTimeoutW](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-sendmessagetimeoutw),
[SetForegroundWindow](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setforegroundwindow).
