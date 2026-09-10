# Directory Change Registration Lifetime and Waiting

## Decision

Keep the long-lived `DirectoryChangeGenerationMonitor` registration thread.
Use a request-local condition variable with a stop-aware deadline wait instead
of polling a future every 2 ms. Completion, task cancellation, and the existing
five-second deadline end the wait. This decision addresses the agreed waiting
and lifetime scope of [issue #89](https://github.com/SW26010/SpecForge/issues/89).

## Evidence

On the development Windows machine on 2026-09-10, a real Win32 probe registered
first-level directory notifications with `FindFirstChangeNotificationW` and
observed them with `WaitForSingleObject`:

- With the registration thread alive, 20/20 local-folder trials stayed quiet
  before a file creation and signaled after it.
- After joining the registration thread, 20/20 trials were already signaled
  before any directory modification. A live handle alone did not preserve an
  unchanged-directory observation boundary.
- A user-selected OneDrive directory on local C: showed the same 20/20 lifetime
  behavior without intentional modifications to that directory.
- Across 1,000 registrations, the observed maxima were 0.3202 ms for the local
  fixture and 0.3049 ms for the OneDrive directory. These measurements do not
  establish a general latency bound or cover disconnected network shares.

Source loading workers can exit while their published listing generations are
still cached. Registering directly on those workers would therefore invalidate
otherwise reusable listings. `TestNativeGenerationSurvivesPreparationCallerThread`
preserves this requirement using the real API, a temporary local directory,
an exited requesting thread, and a subsequent directory modification.

## Cancellation and Ownership

Each source task owns one stop source, shared with its completion bookkeeping.
Cancellation and queue shutdown request that same stop state. Preparation passes
its token to the monitor alongside its existing throwing checkpoint. The token
wakes the condition-variable wait; the checkpoint preserves the source-loading
cancellation exception. Checkpoints run outside the request mutex. Internal stop
callbacks only wake waits and must not re-enter the load queue, whose mutex may
be held while cancellation is requested.

Request completion, errors, and abandonment are protected by the request mutex.
Canceled or timed-out requests still in the queue are skipped. If registration
has already started, a late result is closed on the registration worker instead
of being published. Factory exceptions continue to propagate to waiting callers.

The monitor retains ownership of published registrations and closes them on its
worker when reaping unused leases or shutting down. Surviving leases become
invalid on monitor shutdown. `IsCurrent()` remains a one-shot observation of
whether the first-level directory generation has changed. Unavailable or timed-out
registration still uses post-decode full scan-and-compare; the existing bounded
preparation retry behavior remains unchanged.

## Limits and Validation

The five-second deadline bounds the caller's wait, not the native registration
operation. The production Win32 call does not consume a C++ stop token. A native
call that never returns can still delay monitor destruction, which joins its
worker, and prevent subsequent registrations from executing. This change does
not claim to solve that pre-existing case. A cooperative blocked factory tests
shutdown; it is not evidence of native I/O cancellation.

The [Win32 API documentation](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-findfirstchangenotificationw)
does not specify a registration latency bound and warns that remote file systems
may not deliver notifications. Supporting a hard shutdown bound for stuck remote
registration requires separate platform evidence and an explicit isolation or
fallback decision. OneDrive-on-C: measurements are not an SMB fault test.

Focused tests cover real local notification lifetime, normal failure and factory
exceptions, pre-cancellation, skipping canceled queued requests, task cancellation during blocked registration,
five-second timeout without repeated checkpoints, late-result closure, monitor
shutdown, and the existing listing reuse and full-revalidation fallback paths.
