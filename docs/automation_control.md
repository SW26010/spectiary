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

The two-process labeling smoke runner is the one narrow fixture exception. It
launches two direct GUI copies with
`--automation-allow-persistent-labeling-outputs` and a runner-owned shared
state root, so the production controller can exercise output-backed task
leases. This is an explicit startup opt-in, not a client protocol command; the
normal launcher and all automation startups without the option retain the
`InternalDraftsOnly` seed policy above.
The runner also requires each formal label terminal to report both output-save
success flags, reloads the final NPY and metadata through the production reader,
and bounds pipe I/O and owned-process cleanup so a stalled GUI cannot leave its
fixture behind.

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
panel get <files|navigation|annotations|labeling|filters|sorting|smoothing|information|spectral_lines>
panel set <files|navigation|annotations|labeling|filters|sorting|smoothing|information|spectral_lines> <true|false>
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

### Trust boundary

The trusted principal is the interactive Windows user that launched the
automation instance. The current-user DACL and `PIPE_REJECT_REMOTE_CLIENTS`
exclude other ordinary users and remote pipe clients. The random instance ID,
per-launch pipe name, nonce, and exact hello response bind the launcher to the
GUI child it created and reject stale or cross-instance clients.

This is not a sandbox or an authentication boundary against hostile code already
running as the same Windows user, an administrator, or the operating system.
Same-user code may be able to inspect process arguments and the launcher's state
root. The nonce is instance correlation within the local test/debugging contract,
not a secret credential against that stronger threat model.

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
{"type":"hello","request_id":"hello-1","status":"completed","protocol_version":1,"instance_id":"<instance>","capabilities":["state.get","wait.idle","setting.get","setting.set","panel.get","panel.set","source.open","spectrum.goto","label.assign","frame.capture","profile.start","profile.stop","app.quit"],"max_message_bytes":65536,"queue_capacity":32}
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

### Timeout, retry, disconnect, and crash contract

The protocol has no reconnect, replay, or automatic business-command retry.
Before acceptance, a failed request has not entered an App/Shell/Session owner;
a caller may submit a new request with a fresh ID while the connection still
accepts requests. Every syntactically valid observed ID is connection-scoped and
single-use, including IDs attached to semantic failures, `queue_full`, and
`shutting_down`. The one exception is an ID rejected by
`request_limit_reached`: it is not inserted because the bounded ID set is
already full, but the connection stops accepting requests. A caller must never
retry an accepted mutation or infer its outcome from a reused ID.

The protocol does not carry a timeout field. The launcher owns these finite
budgets: initial pipe connection 10 seconds, hello response 5 seconds, and the
response drain for one command or one pipeline 10 seconds. For a pipeline,
that absolute deadline starts before its first request is written and covers
every request write plus the accepted/terminal response drain; a blocked later
write therefore cannot extend or bypass the budget. The command/pipeline
budget is one overall deadline for the accepted and terminal responses; an
`accepted` response does not reset it, and a pipeline's multiple terminal
responses share the same budget. The `disconnect after accepted` harness
control uses the same 10-second response budget while waiting for acceptance.
Normal cleanup waits 5 seconds before forced child termination, and the final
exact-child process wait is 10 seconds.

When a launcher response deadline expires, the launcher closes the pipe,
returns nonzero, and emits a stable diagnostic. It never reconnects or retries
the request. The hello timeout is reported as `Automation hello response timed
out after 5 seconds.`; command and pipeline timeout diagnostics use
`Automation <exchange> response timed out after 10 seconds; terminal outcome
was not observed.`, with `<exchange>` set to `command` or `pipeline`. This
includes a pipeline write that exhausts the shared budget. After a request has
been successfully sent, any response wait that
ends without an authoritative terminal says `terminal outcome was not
observed`, whether or not the independent `accepted` response reached the
launcher. The server may already have queued or claimed the request before
that response was observed, so this is an ambiguity boundary, not a
cancellation or replay claim. If end-of-input's implicit `app.quit` loses its
terminal response, that diagnostic is retained after exact-child cleanup and
the launcher still returns nonzero, even if the GUI exits with code 0. CTest
and CI still own their hard outer test/suite timeouts and kill-on-close process
Job; see [Automation CI](automation_ci.md).

Disconnect cancels work that has not claimed execution. Claimed work remains
owned by the production component and retires from the server only after its
factual completion, even though its response can no longer reach the client.
If the pipe or GUI process is lost before the client observes a terminal, the
client cannot determine from the protocol alone whether a claimed mutation ran.
There is no cross-process exactly-once recovery token. A new launcher uses a new
root, instance ID, nonce, connection, and request-ID namespace; the previous
isolated root is diagnostic evidence, not a replay log.

Before GUI creation, launcher failure removes only the partial root that the
launcher created and pinned. After GUI creation, the root is retained for
diagnosis. Normal launcher cleanup requests `app.quit` when possible and then
uses the exact child process handle. An unexpected launcher exit closes its
kill-on-close Job Object (or leaves termination to the already-owning runner
Job), so cleanup never searches by executable name or PID alone. Production
owners retain their own atomic-write, retry, and startup-recovery behavior; the
control plane does not add a second recovery model.

## Command contracts

Commands with parameters use these stable request shapes:

```json
{"type":"request","request_id":"setting-get-1","command":"setting.get","params":{"name":"ui.language"}}
{"type":"request","request_id":"setting-set-1","command":"setting.set","params":{"name":"ui.scale","value":125}}
{"type":"request","request_id":"panel-get-1","command":"panel.get","params":{"name":"files"}}
{"type":"request","request_id":"panel-set-1","command":"panel.set","params":{"name":"spectral_lines","visible":false}}
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

`panel.get` and `panel.set` expose the fixed production panel set:

| Stable name | Production panel |
| --- | --- |
| `files` | Files |
| `navigation` | Navigation |
| `annotations` | Annotations |
| `labeling` | Labeling |
| `filters` | Filters |
| `sorting` | Sorting |
| `smoothing` | Smoothing |
| `information` | Information |
| `spectral_lines` | Spectral Lines |

`panel.get` completes with
`"result":{"name":"<name>","visible":<boolean>}`. `panel.set` requires an
explicit JSON boolean and completes with the same fields plus `changed` and the
`frame_index` of the normal Shell frame that successfully presented the
requested state. A valid same-value write is successful and reports
`changed:false`.

Every write calls `ShellUi::SetPanelVisibilityForAutomation`, which delegates
to the existing `ApplicationSettings::Apply` and
`ApplicationSettingsIntent::SetPanelVisibility` production seam. The menu and
panel rendering continue to consume that same `ApplicationSettings` view;
automation does not edit the panel cache, manipulate an ImGui window, or retain
a parallel visibility model. A `panel.set`, including an unchanged write,
remains outstanding until a later normal Shell frame consumes the complete
production `PanelVisibilityState` and produces panel-specific presentation
evidence. Showing a panel requires a successful Present from the actual ImGui
viewport to which that panel window was submitted; a successful main-viewport
Present cannot stand in for a detached panel viewport. Hiding a panel completes
only after every viewport known to have presented that panel either presents a
normal Shell frame without it or is torn down. `ShellUi` publishes each panel's
candidate independently after that evidence; an immersive frame does not
consume or publish panel state, and an unrelated viewport Present, Acquire
retry, or Present retry does not complete the request.

Every applied opposite automation write increments that panel's generation.
An older unpresented generation fails with `operation_canceled` even if a later
write eventually returns to the older requested value; compatible same-target
requests may complete from the same presented frame.

A hidden or minimized main window fails before claim and mutation with
`window_not_renderable`. Immersive plot mode similarly fails before mutation
with `panel_not_renderable`. If the window becomes unrenderable or immersive
mode begins after a production mutation but before a qualifying Present, the
complete chain of mutations since that panel's last qualifying presentation is
restored to its pre-chain visibility through the same `ApplicationSettings`
production seam, and the current request then fails with the corresponding
code. Superseding generations therefore transfer rollback ownership instead of
replacing the original baseline. An unexpected failure to restore reports
`panel_rollback_failed` and states that the unpresented chain could not be
restored. These failures do not leave an earlier claimed execution blocking a
following `app.quit`.

A detached viewport that still exists but is minimized is not renderable:
Dear ImGui omits that secondary viewport from both platform rendering and
buffer swap. A same-value request that depends on it fails with
`panel_viewport_not_renderable`; an applied request first restores the complete
unpresented mutation chain and then fails with the same code. Hiding a panel
from a shared detached viewport uses the same rule while that viewport is
minimized, rather than treating an unrelated main-window Present as proof or
mistaking the still-existing viewport for teardown.

If an ordinary `WM_CLOSE`/`WM_QUIT` ends the run loop while a panel mutation
chain is still awaiting presentation, shutdown restores every active chain
through `ApplicationSettings` before stopping the automation server or flushing
local state. Its claimed request receives an `app_shutdown` terminal (or
`panel_rollback_failed` if restoration cannot be verified), and the final panel
cache therefore contains the pre-chain baseline rather than an unpresented
value.

Changed visibility uses the production 500 ms debounce and retry scheduler.
The successful `panel.set` terminal means the production state has been applied
and presented, not that the future debounce deadline has already written the
cache.
`wait.idle` waits for the panel request's presentation terminal but, consistent
with other scheduled local-state work, does not wait for that future persistence
deadline. Normal `app.quit` performs the existing final flush. The production
panel cache path resolves below the launcher-pinned automation state root, so
neither the debounce write nor shutdown flush can target ordinary user state.
An unknown stable name fails after acceptance with `unsupported_panel`;
missing or wrongly typed parameters fail before acceptance with
`invalid_params`.

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
- all nine current production visibility booleans in `state.panels`, keyed by
  the stable panel names above;
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
that is still unwinding. Source, navigation, label, panel-set, and capture
operations are part of the sequence-aware lifecycle. It deliberately does not
wait for a future persistence deadline; `label.assign` reports whether
state/output
persistence completed, was scheduled, or requires retry, while normal
`app.quit` performs the existing final flush. The initial `setting.set`
whitelist uses synchronous production saves, so its terminal—and therefore a
following idle barrier—cannot precede that settings-file write.
`panel.set` is part of the sequence-aware lifecycle until its requested state
has a qualifying normal Shell Present or an unavailable/superseded terminal
restores or accounts for the applied mutation. Its production debounce write
remains future persistence work and follows the same final-flush rule.
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

## Source truth, derived observation, and persisted projection

The control plane does not make `state.get` or its JSON files a second product
model. These layers have different authority and timing:

| Layer | Authority and examples | Automation meaning |
| --- | --- | --- |
| Production source truth | `ApplicationSettings`, Shell/Session controllers, the sample-labeling controller, `ProfileSink`, and the frame-capture pipeline | Commands validate, claim, and mutate only through these existing owners. Operation-specific terminals report facts produced by the owner. |
| Derived observation | `SpecForgeApp::AutomationState()` plus the last successfully presented `ShellAutomationView` | `state.get` is a point-in-time projection. Live source-load state may lead the presented spectrum/label snapshot; capture and profile fields summarize current or last terminal observations. It is neither durable state nor mutation authority. |
| Persisted projection | Production settings, panel, session, labeling, layout, profile, and capture files below the isolated root | Durability follows each production owner's contract. Settings writes are synchronous; panel/session/label state may be debounced or retryable; a label terminal carries persistence flags; `app.quit` drives the ordinary final-flush/shutdown path. |

A successful command terminal therefore means exactly what that command section
states. It does not generally mean that every future debounce deadline has
elapsed or that a later `state.get` must describe an unpresented live change.

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
all nine one-hot panel mappings, exact boolean fields, generation-based
supersession, chain-baseline rollback through the real settings cache,
deterministic panel disconnects before and after execution claim with
production-cache and active-chain settlement checks,
panel-specific main/detached Present and teardown evidence, and
detached-minimized unchanged/applied rollback plus shared-viewport hide
blocking, hidden/minimized/immersive panel rejection followed by normal quit,
an intervening idle barrier, and a real HWND shutdown rollback before final
cache validation,
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
invalid-name/type/value failures, fixed panel reads, changed and unchanged panel
writes, an unsupported-panel failure, a following idle barrier, applied
`state.settings` and `state.panels`, application-rendered capture, production
settings/panel files, and ordinary-state fingerprint preservation. Its
interactive failure-injection
coverage also blocks the production UI-scale path and verifies
`setting_persistence_failed`, retained model/live values, no applied-setting
notification, and a usable idle barrier.

## Security and reliability audit checklist

Issue #22 audited the production implementation and its registered tests. The
table records durable evidence locations rather than line numbers so it remains
useful as the files evolve.

| Boundary or lifecycle rule | Production source | Verification evidence | Documented contract and audit result |
| --- | --- | --- | --- |
| Local-only, single-user, single-client pipe; remote and second clients rejected | `automation_named_pipe.cpp`: `PrepareCurrentUserPipeSecurity`, `AutomationNamedPipeServer::Start` | `TestPipeAclIsCurrentUserOnly`, `TestSingleClientQueueAndLifecycle` in `automation_control_tests.cpp` | **Verified.** See **Trust boundary** and **Transport and protocol**. Same-user hostile code is explicitly outside the boundary. |
| Random instance/nonce handshake and automation-only startup arguments | `automation_launcher_main.cpp`: `RandomHex`, `BuildGuiCommandLine`; `automation_startup.cpp`: `ParseSpecForgeCommandLine` | `TestStartupAndNoActivationContract`; real launcher hello/instance assertions in `automation_launcher_integration_tests.ps1` | **Verified.** Partial, repeated, mismatched, or inherited legacy startup input fails closed before App construction. |
| New launcher-pinned state root, ordinary-root separation, and isolated production paths | `automation_startup.cpp`: `CreatePinnedAutomationStateRoot`, `AutomationStateRootIsIndependent`; `main.cpp`: `PrepareStartup` | `TestStartupAndNoActivationContract`, `automation_state_isolation_tests.cpp`, root-identity scenarios in `automation_launcher_integration_tests.ps1` | **Verified.** The launcher rejects an existing root and same/parent/child overlap; the GUI independently rechecks separation. |
| Seed identity, read-only ownership, materialization, and persistent-output policy | `automation_launcher_main.cpp`: `ValidateLabelingStateSeed`, `MaterializeLabelingStateSeed`; `automation_startup.cpp`: `PinAutomationReadOnlyFile`, `MaterializePinnedAutomationSeed` | seed replacement/output/reparse scenarios in `automation_control_tests.cpp`, `automation_state_isolation_tests.cpp`, and `automation_launcher_integration_tests.ps1` | **Verified.** Normal launcher seeds permit internal drafts only; the direct two-process fixture requires an explicit startup opt-in. |
| Capture path ownership, create-new/no-follow encoding, and publish/disconnect ordering | `automation_startup.cpp`: `ValidateAutomationCapturePath`; `d3d11_frame_capture.cpp`: handle-relative prepare/publish; `automation_named_pipe.cpp`: `TryFinalizeFrameCapture` | `TestFrameCaptureFinalizationLease`; real GUI outside-root, reparse, disconnect, and PNG checks in `automation_launcher_integration_tests.ps1` | **Verified.** No desktop screenshot or general external write seam exists. |
| Profile output ownership, no-follow final handle, and finalized writer terminal | `automation_startup.cpp`: `AutomationProfileOutputFactory::Create`; `specforge_app.cpp`: `ServiceAutomationProfileStart`, `PollAutomationProfileStop` | `TestProfileOutputCreationIsHandleBoundToAutomationRoot`, `TestProfileWriteFailureUsesStopTerminalResponsePath`, real GUI profile scenarios | **Verified.** Final success requires the summary record and flush; start or write failures remain distinct. |
| UTF-8 JSON, duplicate/nesting/malformed/oversized input, exact commands and bounded response | `local_user_state_json.cpp`: `JsonParser`; `automation_protocol.cpp`: `ParseAutomationClientMessage`; `automation_named_pipe.cpp`: `ReadMessage`, `EnqueueResponseLocked` | `TestProtocolAndStableState`, `TestSingleClientQueueAndLifecycle`, `TestPreHandshakeJsonNestingIsBounded`, `TestQueueCapacityVersionAndDisconnect`, `TestOversizedTerminalResponseIsBounded` | **Verified.** Invalid or unsupported input fails before acceptance; oversized correlated output becomes `response_too_large` without poisoning the connection. |
| Queue, request-ID, acceptance, terminal, and shutdown rejection | `automation_named_pipe.cpp`: `HandleClientMessage`, `TryReserveRequestIdLocked`, `Complete`, `Fail` | `TestSingleClientQueueAndLifecycle`, `TestDistinctRequestIdLimit`, `TestQueueCapacityVersionAndDisconnect` | **Verified.** While connected, each accepted request has one terminal; valid observed IDs are consumed as documented. |
| Execution claims, disconnect, quit sequence barrier, and publication races | `automation_named_pipe.cpp`: `TryClaimExecution`, `TryBeginAppQuit`, `HandleDisconnect`, `TryFinalizeFrameCapture` | `TestExecutionClaimsAndQuitBarrier`, `TestPanelDisconnectBeforeAndAfterClaimSettlesProductionState`, `TestFrameCaptureFinalizationLease` | **Verified.** Unclaimed work cancels; claimed mutations keep factual ownership and terminal ordering. |
| `wait.idle`, retry ownership, and persistence timing | `specforge_app.cpp`: `ServiceAutomation`, `AutomationBusinessIdle`; production persistence owners under `src/app` and `src/sessions` | `TestIdleWaitIsAnEarlierOnlySequenceBarrier`, `TestIdleWaitStopsLaterBusinessDispatch`, real GUI persistence assertions | **Verified.** The barrier is earlier-sequence-only and does not wait for future debounce/retry deadlines. |
| Source truth versus live, presented, terminal, and persisted projections | `specforge_app.cpp`: command service/poll methods and `AutomationState`; `automation_state.cpp`: `SerializeAutomationStateBody` | protocol state assertions in `TestProtocolAndStableState`; real GUI source/goto/label/capture/profile/state workflows | **Verified.** See **Source truth, derived observation, and persisted projection**. |
| Timeout, crash ambiguity, exact-child cleanup, and retained diagnostics | `automation_launcher_main.cpp`: `LauncherChildJobGuard`, `LauncherOwnedProcessGuard`, deadline-bound response helpers; `automation_named_pipe.cpp`: `AutomationNamedPipeClient::SendUntil` and `ReceiveUntil`; `run-automation-ci.ps1`: `Invoke-BoundedCTest` | hello-no-response, no-accepted/accepted-command, accepted pipeline, pipeline-write-stall, and EOF app.quit-no-terminal fake GUI/pipe fixture cases, with retained-root and bystander assertions in `automation_launcher_integration_tests.ps1`; CTest/CI timeout properties | **Verified.** Launcher-owned response/write deadlines close the pipe and preserve the existing exact-child handle/Job Object cleanup path; accepted requests remain explicitly outcome-ambiguous and are never retried. |
| Orderly shutdown, panel rollback, state flush, and writer retirement | `specforge_app.cpp`: run-loop shutdown and `Shutdown`; `automation_panel_command_coordinator.cpp`: `SettleForShutdown` | panel coordinator tests, real HWND shutdown rollback, profile quit-during-stop/recording scenarios | **Verified.** Rollback precedes server stop and local-state flush; normal quit retains production shutdown ownership. |

The 2026-08-06 audit at base commit `4472fad` found no P0 or P1 defect and one
P2 defect, tracked by Issue #30. This follow-up closes that audit gap with the
launcher deadlines and fake-GUI regression described above. The repository
wrapper rebuilt the control, panel-coordinator, state-isolation, launcher, and
native targets successfully.
`ctest -L ci-headless` passed all 5 tests, and the real-GUI
`specforge_automation_launcher_integration_tests` passed its complete
launcher-to-GUI workflow.
