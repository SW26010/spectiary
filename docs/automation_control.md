# Local automation control plane

SpecForge provides a Windows-local control plane for tests and debugging. It is
disabled during ordinary startup and is not a supported end-user CLI, remote
service, or plugin interface.

## Launch

Build the GUI and console launcher with the repository wrapper:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\build-ninja-msvc-debug.ps1 -Target specforge_native
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\build-ninja-msvc-debug.ps1 -Target specforge_automation_launcher
```

Start an isolated automation instance:

```powershell
.\build\ninja-msvc-debug\SpecForgeAutomation.exe
```

The launcher accepts `--app <SpecForge.exe>`,
`--state-root <new-absolute-directory>`, and the optional
`--labeling-state-seed <absolute-production-cache.json>`. The state-root path
must not already exist. When omitted, the launcher creates a unique directory
below the system temporary directory. The launcher also generates a
cryptographically random instance ID and nonce, constructs the per-instance
named-pipe name, and passes all four values to the GUI through explicit
automation-only startup arguments. Those GUI arguments are an internal launcher
contract.

The labeling seed is automation-launch preparation, not a label mutation
command or a profile importer. The launcher requires an existing regular file
outside both ordinary and automation state roots and pins that read-only file
identity while the production labeling-cache reader validates and materializes
it. Automation seeds are deliberately narrower than ordinary user caches: every
task must be an internal draft and every `output_path` must be `null`. Any
non-null output reference is rejected before the production reader hydrates or
accesses it, and the GUI's background production load applies the same policy.
This avoids turning automation seeding into a general output import/write
interface.

The launcher creates the new root relative to a verified directory handle,
copies the seed handle-to-handle with create-new/no-follow semantics, and
validates the materialized `sample-labeling-tasks.json` with the same reader.
A launcher-owned identity lock prevents the root from being renamed or replaced
through the complete GUI child lifetime while permitting normal sibling cache
renames; it is removed after that child exits. This entire sequence finishes
before `CreateProcessW`. Any validation or materialization failure prevents GUI
startup and removes the partial launcher-owned root. The seed is never modified,
and any existing target root is rejected.

Before creating the directory, the launcher resolves the target GUI's ordinary
Standalone/Portable state root and rejects equal, parent, or child paths. A
rejected path is never created or used for diagnostics.

The launcher owns only the GUI process it creates. Every post-launch error path
first requests normal `app.quit` when a completed handshake still permits
communication, then waits for a bounded interval. A disconnected or
unresponsive launcher-owned child is terminated through that exact process
handle; launcher cleanup never searches for or stops unrelated SpecForge
processes.

The thin console accepts only:

```text
setting get <ui.language|ui.scale>
setting set <ui.language|ui.scale> <value>
source open <absolute-file-or-directory>
spectrum goto <zero-based-index>
spectrum goto name <exact-name>
label assign <integer-code>
label assign <integer-code> spectrum <zero-based-index>
label assign <integer-code> spectrum name <exact-name>
frame capture <absolute-new-png-under-state-root>
profile start
profile stop
state get
wait idle
app quit
help
pipeline begin
... one or more commands ...
pipeline end
disconnect after accepted
... exactly one command ...
```

It translates these fixed commands to JSON. The GUI never parses the console's
free-form text. For `setting set`, an optional single leading `+` or `-`
followed by decimal digits becomes a JSON integer when the complete token fits
in signed 64-bit range. An overflowing or otherwise malformed numeric token
remains a JSON string. The ASCII tokens `true` and `false`, matched
case-insensitively, become JSON booleans; other text remains a JSON string. The
GUI still enforces the fixed setting whitelist and each setting's production
type and value validation. `pipeline` and `disconnect after accepted` are
launcher-owned transport/concurrency test controls; they do not add GUI commands
or business seams. A pipeline is limited to the advertised 32-command queue
capacity and is rejected before its first request is sent when that bound is
exceeded. End of input asks the app to quit normally. An attached Windows console
is read as UTF-16 and converted strictly to UTF-8. Redirected standard input,
including all three launcher modes, must already be well-formed UTF-8; bytes in
the active OEM or ANSI code page are not accepted as an implicit encoding.

## Transport and protocol

Version 1 uses a local, message-mode Windows named pipe carrying UTF-8 JSON
objects. There is no TCP listener, fixed discovery pipe, registry rendezvous,
global input simulation, or foreground-window activation. The pipe DACL grants
access only to the current Windows user and rejects remote clients.

The server permits one connection for the instance. A second connection is
rejected; there is no multi-client scheduling or reconnect contract. Each
message is limited to 65,536 bytes, the accepted-command queue is limited to 32
requests, and one connection is limited to 4,096 distinct request IDs.
`request_id` is 1–64 ASCII letters, digits, `.`, `_`, or `-`, and may not be
reused on the connection. Once a valid envelope ID is observed, it is consumed
even when message-type or command validation fails.
After the 4,096-entry set is full, a new ID is rejected with
`request_limit_reached` without being inserted, so retrying that rejected ID
returns the same limit error. Reusing an ID that was consumed before exhaustion
continues to return `duplicate_request_id`.

The first message must be the launcher handshake:

```json
{"type":"hello","request_id":"hello-1","protocol_version":1,"nonce":"<launcher nonce>"}
```

A successful response reports the protocol and fixed capabilities:

```json
{"type":"hello","request_id":"hello-1","status":"completed","protocol_version":1,"instance_id":"<instance>","capabilities":["state.get","wait.idle","setting.get","setting.set","source.open","spectrum.goto","label.assign","frame.capture","profile.start","profile.stop","app.quit"],"max_message_bytes":65536,"queue_capacity":32}
```

Commands use protocol names rather than console spelling:

```json
{"type":"request","request_id":"request-1","command":"state.get"}
```

While its client connection remains alive, an accepted command receives an
`accepted` response and exactly one terminal `completed`, `failed`, or
`canceled` response carrying the same request ID. Disconnect retires requests
under the server synchronization boundary, but responses that could no longer
reach that client are discarded.
Malformed input and commands rejected before acceptance receive only `failed`.
Stable error codes cover invalid input, handshake or version/nonce mismatch,
duplicate IDs, queue/request limits, shutdown rejection, and cancellation.
If a correlated response would exceed `max_message_bytes`, the server replaces
it before pipe write with a bounded terminal `failed` response carrying
`response_too_large`; the connection remains usable.
Mutating requests enter an execution claim under the same pipe-server
synchronization boundary before their first real App/Shell/Session mutation.
Disconnect and `app.quit` cancel only requests that have not claimed execution.
An `app.quit` behind an earlier claimed request is a sequence barrier: it waits
for that request's factual terminal, then claims shutdown, cancels remaining
unclaimed requests, and stops dispatching later commands from the same batch.
Thus a canceled terminal cannot describe a mutation that was already applied,
and a mutation that did occur reaches its matching terminal before the later
quit terminal.

## Command contracts

Commands with parameters use these stable request shapes:

```json
{"type":"request","request_id":"setting-get-1","command":"setting.get","params":{"name":"ui.language"}}
{"type":"request","request_id":"setting-set-1","command":"setting.set","params":{"name":"ui.scale","value":125}}
{"type":"request","request_id":"open-1","command":"source.open","params":{"path":"C:\\fixtures\\spectra"}}
{"type":"request","request_id":"goto-1","command":"spectrum.goto","params":{"target":{"index":4}}}
{"type":"request","request_id":"goto-2","command":"spectrum.goto","params":{"target":{"name":"target.csv"}}}
{"type":"request","request_id":"label-1","command":"label.assign","params":{"code":5}}
{"type":"request","request_id":"label-2","command":"label.assign","params":{"code":5,"target":{"index":4}}}
{"type":"request","request_id":"capture-1","command":"frame.capture","params":{"path":"C:\\automation-state\\artifacts\\target.png"}}
{"type":"request","request_id":"profile-start-1","command":"profile.start"}
{"type":"request","request_id":"profile-stop-1","command":"profile.stop"}
```

Fixed parameterless commands accept an omitted `params` member or an empty
object only. Any member is rejected before acceptance with `invalid_params`;
no field is silently ignored.

`setting.get` and `setting.set` expose only this initial stable whitelist:

| Name | JSON value type | Supported values |
| --- | --- | --- |
| `ui.language` | string | exact persisted values `en` or `zh-Hans` |
| `ui.scale` | integer | 80 through 150, inclusive |

`setting.get` completes with
`"result":{"name":"<name>","value":<current-value>}`. `setting.set`
completes with the same fields plus boolean `changed`; an unchanged valid write
is successful and reports `changed:false`.

Writes route through the same `ApplicationSettings::Apply` owner used by the
Settings UI. That owner performs the existing validation and save-before-publish
transition. The App then consumes the existing language/UI-scale notification,
updates the localized UI or live ImGui scale, and requests a frame before the
terminal response is completed. There is no automation settings copy and no
direct settings-file mutation. Both production settings paths resolve below the
launcher-pinned automation state root. The same App-owned drain runs before
automation dispatch and state observation, so a notification produced by the
Settings UI cannot be skipped by a same-value `setting.set`, `state.get`, or
`wait.idle`.

Missing or non-scalar parameter structure fails before acceptance with
`invalid_params`. After acceptance, an unknown name fails with
`unsupported_setting`, a scalar of the wrong production type fails with
`setting_type_mismatch`, a correctly typed but unsupported or out-of-range value
fails with `setting_value_rejected`, and a production write failure fails with
`setting_persistence_failed`. Failed writes retain the prior value and do not
emit an application setting notification. Consequently, a CLI integer token
outside signed 64-bit range remains a string and fails integer-valued
`ui.scale` with `setting_type_mismatch`; a representable integer outside
80 through 150 fails with `setting_value_rejected`.

`source.open` requires an absolute existing regular file or directory accepted
by the production source loader. Its terminal result contains stable source
identity/path/count and the initially presented spectrum index/name. The
request remains outstanding through background preparation, UI-thread session
activation, draw submission, and a successful application Present. A newer
activation generation may cancel an older operation even when the replacement
has the same path, source ID, and row. Logical open identity is retained across
production follow-up loads, so the latest same-path open does not cancel itself.
A real GUI open also cancels a pending automation open; its retired worker
completion cannot later enter the session or reactivate that source.
Stable command errors are
`path_not_absolute`, `source_not_found`, `source_load_failed`, and
`operation_canceled`; malformed JSON parameters use `invalid_params`.

`spectrum.goto` requires exactly one target identifier: a zero-based source row
`index`, or a non-empty Windows Unicode ordinal, case-insensitive exact `name`.
Signed indexes are invalid launcher syntax and never become requests. Indexes
outside the source fail with `spectrum_index_out_of_range`. Name lookup distinguishes
`spectrum_name_unavailable`, `spectrum_not_found`, `spectrum_ambiguous`, and
`spectrum_filtered_out`. Source-row identity is independent of the current sort:
an included row remains addressable by index or name in a filtered or sorted
sequence, while only a row excluded from the active sequence is
`spectrum_filtered_out`. With no active/ready source it returns
`no_active_source` or `source_not_ready`. Successful navigation, including an
unchanged same-row target,
completes only after the requested spectrum is current, Shell is idle, and the
exact source ID and row have been submitted by Draw and observed through a
successful Present. Acquire/Present retry does not complete the command. The
Draw/Present observation carries the activation generation captured at Draw;
same-source/same-row presentation from an older activation cannot complete the
request. The result includes source ID, spectrum index/name, and `changed`. A changed
navigation that cannot load its row fails with `spectrum_load_failed`; a
superseding source activation fails it with `operation_canceled`.

`label.assign` takes a label code and optionally the same spectrum target. It
uses the active production labeling task and retains its configured
auto-advance/overwrite behavior. It fails deterministically with
`no_current_spectrum`, `no_active_label_task`, `label_not_found`, or
`label_assignment_rejected` (plus the navigation errors above for an explicit
target, including `spectrum_load_failed` and `operation_canceled`). A targeted
assignment does not write until the exact target source/row has a successful
Draw/Present observation for the same activation generation. If a replacement
activation wins first, the request is canceled without writing. Once the real
label write begins, its terminal remains factually tied to that write even if a
later quit, disconnect, or source activation occurs. A later activation cannot
leave the request waiting for an impossible Present from the replaced source;
the completed terminal is emitted from the stored production assignment fact.
The terminal result records the spectrum that was actually written,
task ID, previous/new code, `changed`, production persistence status/flags, and
the current spectrum after any auto-advance. The caller must explicitly
`spectrum.goto` back to the written row before asserting its current label;
terminal assignment identity is not inferred from post-advance state.

`frame.capture` is a test-control safety contract scoped to the current
automation state root, not a general external file-write interface. The caller
may choose any new absolute `.png` below that root. Existing output paths,
paths through reparse points, other extensions, relative paths, and paths
outside the root are rejected with `capture_output_exists`,
`capture_path_outside_state_root`, `capture_path_invalid`, or
`capture_path_not_absolute`. For automation capture, missing parent components
are opened or created one component at a time relative to the pinned root and
without following reparse points; there is no path-based recursive directory
creation after validation.

Capture uses the application's existing Direct3D 11/WIC seam after
`ImGui_ImplDX11_RenderDrawData` and before the main viewport Present. A
successful result contains normalized path, `png`, `main_viewport`, frame
index, width, and height. There is no desktop/window screenshot API and no
cached frame fallback. WIC encodes to `.tmp` outside the pipe lock; the final
no-replace `.tmp` to PNG publication, exact request/sequence validation, and
terminal transition share a short pipe-server finalization lease. The temporary
name is random and created with create-new/no-follow semantics; WIC writes
through an `IStream` bound to that file handle. Each directory component is
opened without following a reparse point, and the pinned root, parent, and
published file identities are revalidated around a directory-handle-relative
no-replace rename. A dangling legacy `<output>.tmp` entry is never opened or
followed. If
disconnect or quit wins that boundary, the temporary file is removed and no
PNG is published. A hidden or minimized window fails with
`window_not_renderable`; it is never restored or forced to draw. Other stable
capture errors are `capture_busy`, `capture_directory_failed`, and
`capture_failed`.

`profile.start` and `profile.stop` control the production performance recorder
used by Settings > Diagnostics. They are fixed parameterless commands: the
control plane does not create an automation-only recorder and does not accept a
caller-selected output directory or file path. Their `params` member may be
omitted or an empty object; any member is rejected with `invalid_params`.
`profile.start` first resolves
the normal production profile output directory from the isolated instance's
`ApplicationSettings`, then requires that resolved directory to remain strictly
below the launcher-pinned automation root without existing reparse points. It
then reopens the isolated root and every output-directory component with
no-follow, directory-handle-relative operations. The final JSONL is opened with
create-new/no-follow semantics relative to the pinned output-directory handle;
the root, directory, and final-file identities are checked before the resulting
file handle is transferred to `ProfileSink`. `ProfileSink` writes and performs
its final flush through the stream that owns that exact handle, so a dangling
final-file symlink or a validation-to-open directory replacement cannot redirect
the recording. The App's existing `StartProfileRecording` UI-thread seam remains
the owner of production status, events, limits, and writer lifetime. The command
completes only after that production writer owns the real JSONL output. Its
result is:

```json
{"result":{"status":"recording","path":"C:\\automation-state\\logs\\specforge-profile-<timestamp>.jsonl"}}
```

An already active or finishing recording fails with
`profile_recording_active` or `profile_stop_in_progress`. A resolver outside the
isolated root fails with `profile_output_outside_state_root`; a production start
failure uses `profile_start_failed`. Each start attempt replaces the previous
terminal observation. If it fails before `ProfileSink` accepts a file,
`state.get` reports `status:"failed"`, `stop_reason:"none"`, zero dropped
events, and no profile path; it never reuses or later restores metadata from a
previous successful recording.

`profile.stop` enters the same mutation claim used by other factual App
operations, writes the existing stop event, and calls
`ProfileSink::RequestStopAfterFrame()`. Automation command dispatch occurs
outside the active render-frame admission guard, so this command does not
promise an additional render-frame tail: admission closes and the existing
queued events drain asynchronously. The request does not complete merely
because stopping was requested. Completion requires `TryFinalizeStop()` to join
the finished writer and therefore implies that the JSONL's final
`profile_recorder_summary` record and final flush have succeeded. The completed
result is:

```json
{"result":{"status":"succeeded","path":"C:\\automation-state\\logs\\specforge-profile-<timestamp>.jsonl","stop_reason":"explicit","dropped_events":0}}
```

A final stream failure produces terminal `failed` with
`profile_write_failed`, never a successful saved result. The stable
`dropped_events` count describes bounded-recorder pressure; a nonzero value does
not mean the final file flush failed. `profile_not_recording` rejects a stop
without an active session, and a concurrent stop uses
`profile_stop_in_progress`. Duration and file-size limits retain their existing
production stop reasons and are observable through `state.get` even when no
explicit stop request owns the terminal.

Only one automation navigation/label transition is active at a time; a
conflicting request fails with `operation_busy`. Source opens retain the real
source-activation generation/cancellation semantics.

`state.get` returns only stable observation fields:

- protocol and instance/handshake state;
- control acceptance, pending/outstanding counts, and capacity;
- aggregate Shell idle plus source-load, pending-completion, and background-
  retirement idle components and their counts (`state.shell.idle` retains
  aggregate Shell semantics and does not fold control/business requests into
  that field);
- the existing live activated source identity/path in `state.source`, preserving
  the original field semantics even before that source has presented;
- the applied `state.settings.language` and
  `state.settings.ui_scale_percentage` UI observation;
- the last successfully presented source identity/path in
  `state.presented_source`;
- successfully presented spectrum count and, when present, its zero-based
  index and name;
- the labeling task identity and spectrum label captured from that same
  successful Draw/Present;
- capture `pending`, the pending `current_path` when present, and terminal
  `last_result` (`none`, `succeeded`, `failed`, or `canceled`) plus `last_path`;
- profile `status` (`inactive`, `recording`, `stopping`, `succeeded`, or
  `failed`), `stop_reason`, `dropped_events`, and `path` once a production path
  has been selected;
- window visibility, minimized state, and client-area dimensions;
- running/shutdown flags and frame index.

`state.spectrum` and `state.labeling.current_spectrum_label` always describe the
`state.presented_source` draw snapshot. Source completion drained during
maintenance, or a frame whose Present must retry, may advance the separate live
`state.source`, but does not advance the presented source/spectrum/task/code
projection. The label is not “the most recent assignment”; no last-assignment
state is retained.
If a second capture is rejected with `capture_busy`, the first request remains
`pending`/`current_path`, while `last_result=failed` and `last_path` identify
the rejected second attempt until the pending capture reaches its own terminal.

`wait.idle` completes when all earlier accepted control requests are terminal,
and the Shell work attributable to that earlier sequence reports source
loading, pending completion, and background retirement idle. Later accepted
requests do not delay or cancel that barrier: the pipe may accept them, but the
UI dispatch queue stops at the first `wait.idle` and does not dispatch later
commands until that wait reaches its terminal. This makes the wait a real UI
dispatch barrier, so later source work cannot mask an earlier canceled worker
that is still unwinding. Source, navigation, label, and capture operations are
part of the sequence-aware lifecycle. It deliberately does not wait for a
future persistence deadline; `label.assign` reports whether state/output
persistence completed, was scheduled, or requires retry, while normal
`app.quit` performs the existing final flush. The initial `setting.set`
whitelist uses synchronous production saves, so its terminal—and therefore a
following idle barrier—cannot precede that settings-file write.
An active performance recording is continuous observation rather than pending
business work, so it does not keep `wait.idle` open. A claimed `profile.stop`
does remain an earlier outstanding operation until the output has factually
finished, so a later idle barrier cannot pass its final flush.

`app.quit` posts the normal window close path. Existing application flush and
Shell shutdown own persistence and worker retirement; the control plane does
not bypass them. Quitting during an active recording synchronously drains the
existing recorder during App shutdown. A quit queued behind a claimed
`profile.stop` waits for that stop's terminal first; neither case leaves a
writer or partial final record after the launcher-owned GUI process exits.

Named-pipe I/O runs away from the UI thread. It may validate and enqueue
requests, but all App, Shell, Session, ImGui, and HWND state observation or
mutation occurs from the existing UI-thread event loop. Control readiness wakes
that loop with an application message, and idle checks use a bounded maintenance
deadline, preserving event-driven rendering.

## State and focus isolation

Automation redirects all SpecForge-owned mutable state paths beneath the
launcher-created root. Startup rejects a root equal to, containing, or contained
by the ordinary Standalone/Portable state root. Ordinary startup remains
unchanged. Automated startup errors are written inside the automation root
instead of opening a modal dialog, but only after that root has passed the
isolation check.

The launcher passes an explicit child environment with the legacy
`SPECFORGE_PROFILE`, `SPECFORGE_PROFILE_DIR`,
`SPECFORGE_RUNTIME_RESOURCE_WORKLOAD`, and
`SPECFORGE_RUNTIME_RESOURCE_STATE_DIR` variables removed. The GUI independently
rejects automation startup if any of them is present, before App/Shell state is
constructed.

The cleared environment is not restored for profile automation. Performance
recording starts only through `profile.start`, uses the isolated instance's
production directory resolver, and is rejected if that resolver no longer
points below the automation root. Ordinary profile settings and output remain
untouched.

The launcher requests `SW_SHOWNOACTIVATE`; the GUI uses the narrow
`SWP_NOACTIVATE` show seam. The successful capture workflow requires the main
window to be visible and not minimized, but it does not activate or foreground
that window. Automation does not call `SetForegroundWindow`.

The CTest coverage includes protocol and pipe boundaries, current-user ACL,
second-client rejection, queue/full and duplicate-ID handling, version
mismatch, disconnect and shutdown cancellation, a controlled no-activation
show-plan assertion, bounded setting get/set parsing and execution claims,
both disconnect-first and finalizer-lease-first capture
publication ordering, and a real SpecForge HWND workflow that verifies visible
capture without foreground activation plus hidden/minimized rejection without
window restoration. Coverage also includes runtime-state isolation and a
real launcher/GUI profile-start/source-open/wait/goto/label/return/capture/
profile-stop/state/quit integration plus a second fresh-root replay of the core
source/label/capture workflow. Profile-specific coverage also exercises start/
stop conflicts, stable recording/stopping state, production start failure, and
quit during both recording and asynchronous stop. The primary GUI workflow
requires `window.visible=true` and `window.minimized=false`, validates a
non-empty PNG IHDR whose dimensions match the terminal, reloads the production
labeling cache after quit, verifies the finalized JSONL summary and isolated
handle-bound path, and checks the seed and ordinary root fingerprints. Security
regressions verify create-new/no-follow rejection of a real dangling final
reparse entry and a directory-swap junction without changing the external-
directory fingerprint. The final-entry fixture uses a file symlink when the
Windows token has that capability; otherwise it reports the missing capability
and uses a real dangling final-component junction. It never substitutes an
ordinary name-collision file for reparse-point coverage.
The factory regression also holds the same live pinned-root lease used by the
launcher, verifies profile creation remains compatible with that lease, and
confirms the root still cannot be replaced.
Additional integration coverage proves the root cannot be replaced during the
child lifetime, exercises explicit UTF-8 Unicode source/name input plus seed
and capture-path failures, and checks that no owned GUI process remains. A
separate real GUI settings
workflow verifies the initial isolated values, language and scale writes,
including a leading-plus same-value write returning `changed:false`, stable
invalid-name/type/value failures, a following idle barrier, applied
`state.settings`, application-rendered capture, production settings files,
and ordinary-state fingerprint preservation. Its interactive failure-injection
coverage also blocks the production UI-scale path and verifies
`setting_persistence_failed`, retained model/live values, no applied-setting
notification, and a usable idle barrier.
