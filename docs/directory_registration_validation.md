# Directory Registration Validation (#89)

This investigation distinguishes notification lifetime, caller responsiveness,
and native-call cancellation. It does not change the production implementation.

## Reproduce the native experiment

From the repository checkout, supply an absolute path to an existing directory:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/probe-directory-change-registration.ps1 -DirectoryPath 'C:\path\to\test-directory'
```

The script does not enumerate, create, edit, or remove files in that directory.
It registers and closes 1,000 native notifications, then runs 20 paired lifetime
trials. In each trial it observes the **same handle** while the registering thread
is alive and again after joining that thread. Use a quiet directory: unrelated
changes, including cloud synchronization, can also signal the handle.

The PowerShell parent starts a hidden child process and bounds the whole probe
to 20 seconds by default. It performs no directory stat before starting that
child. On timeout it requests termination of that specific child, waits up to
five more seconds, and reports whether termination completed. This is a test
watchdog, not a guarantee that Windows can terminate every stuck driver I/O.
JSON results and stderr are retained under `logs/directory-registration/`.

`-Count`, `-LifetimeTrials`, and `-TimeoutSec` configure the experiment. Registration
failure is reported by `RegistrationError` and `SuccessfulRegistrations`; process
exit zero means the probe ran, not that registration succeeded. Timing is for
registration only, excluding close and thread creation. No timings are inferred
from a watchdog timeout because compilation and lifetime trials are also bounded
by it.

## Results on 2026-09-10

| Environment | Registration | Median / p95 / maximum | Paired quiet-before-exit, signaled-after-exit |
| --- | --- | --- | --- |
| Empty local fixture under repository logs | 1,000/1,000 succeeded | 0.0067 / 0.0076 / 0.3116 ms | 20/20 |
| User-selected OneDrive directory on C: | 1,000/1,000 succeeded | 0.0088 / 0.0110 / 0.3096 ms | 20/20 |
| Nonexistent share on `localhost` | Failed immediately with Win32 error 67 | 2.4898 ms for one attempt | Not run |

Raw result IDs in the local logs are respectively
`177bffad08fd400e8768e25618087f3b`,
`d8009c8eb04e4f16ac322cb500df8a06`, and
`ebc68987d14843d2a3bde6f8946690e1`.

The watchdog itself was tested with 100 lifetime trials and a five-second limit.
The intentionally long experiment timed out and reported termination confirmed.
That result is **not** a native registration stall.

An initial attempt to create a new temporary, current-user-only, read-only SMB share
over an empty repository fixture failed with Windows access denied. The current
user token was not elevated; only built-in administrative shares were present.
No existing share, service, firewall rule, network connection, or data directory
was modified. That initial attempt did not test successful SMB notifications or
share withdrawal. The subsequent authorized UAC-elevated experiment below closes
those gaps. A missing local share alone only establishes prompt failure behavior
for that particular path.

## One-time SMB share-withdrawal experiment

With explicit user authorization, an isolated temporary SMB share was created
through Windows UAC on 2026-09-10. Only the current user received read access;
its backing directory was a new empty fixture under repository logs. Access was
through `\\localhost\<unique-test-share>`, exercising the local Windows SMB
client/server path without changing any existing shares or disabling networking.

- **Normal registration:** 1,000/1,000 succeeded; median 0.1321 ms, p95 0.2167 ms,
  maximum/first call 3.9901 ms.
- **Notification lifetime:** all 20 paired trials were quiet while the registering
  thread was alive and signaled after that same thread exited.
- **Real directory modification:** a watch initially returned `WAIT_TIMEOUT`;
  creating one file through the fixture's local path caused the SMB notification
  wait to succeed. The notification handle closed successfully.
- **Share withdrawal:** a separate child held a fresh, initially quiet watch and
  attempted registration every 10 ms. The administrator helper removed only this
  test share once at least ten registrations had completed. Across 300 attempts,
  15 succeeded (maximum 1.0868 ms), then 285 returned Win32 error 67
  (`ERROR_BAD_NET_NAME`, maximum 1.4908 ms). The existing watch also became
  signaled. Every successfully opened handle, including the original watch, was
  closed successfully. No registration stall was observed.

This is an abrupt **share unavailability** experiment during an ongoing stream
of registrations. It does not prove that removal occurred inside a particular
native call. It is not a packet-loss, unresponsive-server, or physically
disconnected remote-host experiment: the loopback server remained responsive and
could return an error promptly.

The helper's `finally` cleanup reported no errors: the unique test share was
absent, the backing fixture and its test file were deleted, and all child
processes exited. A separate check confirmed the share and fixture were absent,
the elevated helper had exited, and the original administrative shares remained.
No drive mapping, firewall rule, persistent share, account, or service change
was introduced. One-time helper source files were removed after the run; only
the result evidence remains in `logs/issue89-smb-once/` (`report.json`,
`change.jsonl`, and `disconnect.jsonl`).

## Uncooperative registration test

`TestUncooperativeRegistrationBoundsWaitButDelaysShutdown` uses the production
monitor with a factory that ignores its stop token. A separate test gate releases
the factory; a 15-second emergency release prevents the fixture from blocking
forever if an assertion fails. This simulates a stuck operation without claiming
to reproduce a native Windows stall.

Verified behavior:

- Canceling the caller returns no generation within the one-second test budget,
  while the registration factory remains blocked.
- A subsequent request cannot bypass that single occupied registration worker,
  but its caller still returns unavailable after the five-second deadline.
- Monitor destruction remains waiting during a 100 ms observation window.
- Releasing the factory lets destruction finish within the two-second test budget.
  The late registration is closed and the abandoned queued factory is never run.

The preparation test target built successfully with the required Ninja/MSVC
wrapper and passed CTest in 10.26 seconds, including the existing five-second
timeout test. The measurements show caller responsiveness and the shutdown
limitation separately; cancellation of the caller is not cancellation of the OS
operation.

## Platform documentation and interpretation

Microsoft documents that [thread exit cancels pending I/O not associated with a
completion port](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-exitthread).
Its [directory notification overview](https://learn.microsoft.com/en-us/windows/win32/fileio/obtaining-directory-change-notifications)
describes a notification wait as similar to a pending read against a directory.
Together these support an explanation for the experiment, but the precise
signaled-handle behavior remains an observed platform result, not a quoted
`FindFirstChangeNotificationW` guarantee.

The [registration API documentation](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-findfirstchangenotificationw)
specifies no registration latency bound and warns that remote file systems may
not return notifications. Neither the local timing samples nor the OneDrive
samples prove a bound for arbitrary remote paths.

## Decision supported by the evidence

Keep the long-lived registration owner in the current loading architecture:
registering on a short-lived loading worker loses a useful unchanged-directory
boundary when that worker exits. Keep the narrow registration factory seam to
validate timeout, cancellation, queue blockage, and late-handle closure.

The investigation does not prove that native registration must be asynchronous
solely to avoid blocking. It also does not prove that arbitrary supported paths
are safe for a synchronous call without a deadline boundary. Do not change the
loading architecture or claim universal bounded shutdown from these results.

The remaining network gap is an isolated remote SMB server whose transport can
be interrupted or made unresponsive without affecting real users. The successful
loopback share-withdrawal experiment does not replace that fault model. Keep
timing of registration separate from notification waiting and shutdown. Do not
disable the host network or stop its shared SMB service to manufacture a fault.
