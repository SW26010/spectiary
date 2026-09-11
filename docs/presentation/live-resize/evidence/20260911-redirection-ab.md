# Creation-time redirection A/B evidence — 2026-09-11 08:02/08:03

> Historical investigation record. See the [current resize investigation](../README.md) for current decisions and next steps.

User observation: neither A nor B improved perceived resize stutter; B seemed
to stutter more. The user-visible acceptance criterion is therefore not met.
This experiment must not be promoted to the default presentation configuration.

## Capture integrity

- A: `logs/live-resize-20260911-080224-ca2055ab`, 19.0058209 seconds,
  32,068,028-byte JSONL and 395,313,152-byte ETL.
- B: `logs/live-resize-20260911-080310-4244ebd5`, 24.5882959 seconds,
  55,919,495-byte JSONL and 652,214,272-byte ETL.

Both manifests identify the same isolated executable SHA256:
`9CA9ADA85C7067059F358479B4A2FB00DD9015EE73EE46CA4424DF2D234F75BB`.
Both JSONLs pass detached/native/clock acceptance and their requested A/B arm.
The actual sampled detached HWND style has the bit absent in A and present in
B; the result is not inferred solely from environment settings. Both recorders
report zero dropped events. Both converted ETLs report zero lost events,
`Truncated=False` and `HasCallStacks=True`.

Both use Composition without recorded backend degradation; startup source is
empty. Both report configured dynamic refresh, approximately 60 Hz virtual and
120 Hz physical refresh. Interaction duration and resize counts differ, so the
recordings are not identical workloads, replicated trials, or a statistically
controlled claim that B improves or worsens overall performance.

## What changed in the measured stages

Nearest-rank statistics on frames with a detached renderer resize: 767 frames
in A and 1,457 in B. Durations are milliseconds. Nested spans overlap.

| Stage | A p95 | B p95 | A max | B max |
| --- | ---: | ---: | ---: | ---: |
| Whole RenderFrame | 15.6090 | 14.1356 | 69.7421 | 37.4626 |
| Native size callback | 5.8442 | 1.4996 | 61.0964 | 2.7853 |
| Renderer viewport resize | 4.4898 | 5.7084 | 10.2929 | 14.7906 |
| Detached Present API return | 2.7886 | 3.1585 | 7.3357 | 8.7627 |

Native-size p99 changes from 21.0726 to 1.8074 ms. RenderFrame p99 changes from
28.4034 to 17.4922 ms. These measurements show a much smaller native-size tail
in B while some renderer and Present measures are higher. They do not override
the user's reported experience, measure input-to-displayed-frame latency, or
prove that an increase in a different stage was caused by the flag.

## ETW blocking stacks

Analysis uses the capture-specific clock PID/TID and QPC mapping. For each arm,
the three longest RenderFrames containing detached resize were selected. Their
switch-out stacks are resolved through TraceLog `CSwitchTraceData.BlockingStack`,
using each capture's saved application PDB and the previously matched system
symbol cache. Unresolved vendor/D3D/Composition function names are preserved as
module-only frames; exact API claims below are restricted to resolved frames
and the application source call site.

A reproduces the earlier blocking chain. Frames 472, 558 and 690 have selected
off-CPU intervals of 58.6419, 52.5538 and 47.3445 ms respectively, in:

```text
ImGui_ImplWin32_SetWindowSize -> NtUserSetWindowPos
 -> RecreateRedirectionBitmap -> CreateStandardAllocation
 -> VIDMM_GLOBAL::CommitVirtualAddressRange -> WaitForFences
```

In B, native-size callbacks are all below 2.8 ms in the detached resize set.
The selected longest whole frames expose other blocking paths:

1. Frame 1741 (whole frame 37.4626 ms): a 19.8765 ms off-CPU interval in
   `CollectPresentationUpdate -> TakeCompositionFeedback -> TakeFeedback ->
   DrainStatistics -> dcomp -> NtFlipObjectQueryNextMessageToProducer ->
   CPushLock::AcquireLockExclusive`. The JSONL places a 20.0507 ms boundary gap
   after renderer resize end and before platform update end. This is separate
   from the measured 4.8149 ms renderer resize itself. `DrainStatistics` calls
   `GetNextPresentStatistics` in the current application implementation.
2. Frame 1543: a 10.6742 ms off-CPU interval under `Buffer::Reset -> ResetBuffers`,
   through D3D/vendor frames to `VIDMM_GLOBAL::TerminateAllocation`.
3. Frame 499: a 7.3687 ms interval in `Buffer::Reset -> ResetBuffers`, reaching
   `VidMmFreeGpuVirtualAddress -> QueueSystemCommandAndWaitInternal -> WaitForFences`;
   another 6.6035 ms interval occurs under the application's Composition Present
   through a similar GPU virtual-address release/fence path.
4. Frame 1741 also has a 5.3570 ms off-CPU interval under Composition Present
   reaching the GPU virtual-address release/fence path.

These are selected examples, not an exhaustive census of all waits or proof of
a driver defect. A thread's off-CPU interval includes any scheduling delay before
resumption; this A/B helper does not split ready-queue delay from blocked time.

## Decision and next investigation

The narrow hypothesis is supported: the creation-time bit substantially reduces
the previously identified native-size long tail in these recordings. The broader
hypothesis that this alone resolves perceptible resize stutter failed the user's
acceptance. Keep B experimental and preserve the baseline.

The next instrumentation should bound and time Composition feedback collection
(`DrainStatistics`, its availability check and `GetNextPresentStatistics`) and
retain correlation to resize/frame/viewport. Existing buffer-reset spans and ETW
stacks should then be used to examine synchronous resource retirement. Any change
to feedback polling, buffer reuse/retirement or presentation policy must remain a
separate reviewed experiment; do not silently change it in telemetry work.
Displayed-frame/input latency remains necessary for an eventual UX claim.
No additional identical user capture is needed to establish these findings.

Derived artifacts in each recording directory: `analysis-schema.json`,
`analysis-breakdown.json`, `etl-targets.tsv`, `system-trace.etlx`,
`etl-conversion.log`, `ab-wait-stacks.tsv`, `ab-symbols.log` and
`analysis-ab-waits.json`. Capture-specific helpers are
`logs/prepare-ab-targets.cjs`, `logs/read-ab-waits.ps1` and
`logs/summarize-ab-waits.cjs`. Original ETL and JSONL files are unchanged.
