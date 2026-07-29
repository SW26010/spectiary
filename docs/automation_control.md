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

The launcher accepts `--app <SpecForge.exe>` and
`--state-root <new-absolute-directory>`. The state-root path must not already
exist. When omitted, the launcher creates a unique directory below the system
temporary directory. The launcher also generates a cryptographically random
instance ID and nonce, constructs the per-instance named-pipe name, and passes
all four values to the GUI through explicit automation-only startup arguments.
Those GUI arguments are an internal launcher contract.

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
state get
wait idle
app quit
help
```

It translates these fixed commands to JSON. The GUI never parses the console's
free-form text. End of input asks the app to quit normally.

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
{"type":"hello","request_id":"hello-1","status":"completed","protocol_version":1,"instance_id":"<instance>","capabilities":["state.get","wait.idle","app.quit"],"max_message_bytes":65536,"queue_capacity":32}
```

Commands use protocol names rather than console spelling:

```json
{"type":"request","request_id":"request-1","command":"state.get"}
```

An accepted command receives an `accepted` response and exactly one terminal
`completed`, `failed`, or `canceled` response carrying the same request ID.
Malformed input and commands rejected before acceptance receive only `failed`.
Stable error codes cover invalid input, handshake or version/nonce mismatch,
duplicate IDs, queue/request limits, shutdown rejection, and cancellation.
When `app.quit` is accepted, other queued or waiting requests are canceled.
Disconnecting the client invalidates queued work before it can mutate
application state.

## Command contracts

`state.get` returns only stable observation fields:

- protocol and instance/handshake state;
- control acceptance, pending/outstanding counts, and capacity;
- aggregate Shell idle plus source-load, pending-completion, and background-
  retirement idle components and their counts;
- current source identity and path, when present;
- window visibility, minimized state, and client-area dimensions;
- running/shutdown flags and frame index.

`wait.idle` completes when all earlier accepted control requests are terminal,
the control queue is empty, and the Shell observation reports source loading,
pending completion, and background retirement idle. It deliberately does not
wait for a future persistence deadline.

`app.quit` posts the normal window close path. Existing application flush and
Shell shutdown own persistence and worker retirement; the control plane does
not bypass them.

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

The launcher requests `SW_SHOWNOACTIVATE`; the GUI uses the narrow
`SWP_NOACTIVATE` show seam. Automation does not call `SetForegroundWindow`.

The CTest coverage includes protocol and pipe boundaries, current-user ACL,
second-client rejection, queue/full and duplicate-ID handling, version
mismatch, disconnect and shutdown cancellation, a controlled no-activation
show-plan assertion, runtime-state isolation, and a real launcher/GUI
state-wait-quit integration test that fingerprints the ordinary state root
before and after the run.
