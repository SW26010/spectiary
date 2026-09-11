# Detached resize feedback/release follow-up, 2026-09-11 08:20

> Historical investigation record. See the [current resize investigation](../README.md) for current decisions and next steps.

User observation: **未见明显改善**. B remains an unsuccessful UX experiment,
not a production fix. No resize or presentation policy is changed by this analysis.

## Capture validity

- Directory: `logs/live-resize-20260911-082026-0b99c1d2`.
- Manifest arm B, isolated executable SHA256
  `34A1CB1B08A44BF5B08FAF0F005D787D9F861AD7BCB33595A4DA0C68D04894EE`.
- JSONL contains 17.6461779 seconds and 1,082 completed detached resize frames.
  Recorder reports zero dropped events but `stop_reason=file_size_limit`,
  accepted bytes 104,865,438. This is **partial evidence**, not a complete profile
  acceptance pass. The unrecorded tail may contain worse frames.
- Converted ETL reports zero lost events, no truncation and available call stacks.
  ETL completeness does not repair the missing JSONL tail.
- New feedback span accidentally reused the old `presentation_feedback` aggregate
  name. The original analyzer fails at line 247 on the aggregate's absent span schema.
  The follow-up renames the span `presentation_feedback_collect`, keeps the existing
  aggregate unchanged and adds a regression fixture with both formats. It also
  explicitly rejects recorder file-size termination even if all spans happen to close.

## Completed frames before truncation

These are individual observations, not cross-build improvement statistics.
Nested durations must not be added together.

| Frame | RenderFrame | Selected JSONL spans |
| --- | ---: | --- |
| 1002 | 18.5647 ms | native size 1.2701 ms; resize 5.2388 ms; slot 2 texture release 3.0119 ms; GetNextPresentStatistics 2.3875 ms; detached feedback collection 3.1304 ms; detached Present 2.2698 ms |
| 1470 | 17.9807 ms | resize 7.0201 ms; buffer reset 6.2226 ms; slot 1/2 texture release 2.0237/2.8189 ms; detached Present 3.7129 ms |
| 993 | 17.4270 ms | ETL resolves texture release and Present GPU-address-release waits below |

Clock-correlated CSwitch blocking stacks for these three complete frames:

- Frame 1002: 2.2670 ms off CPU inside the instrumented GetNext call,
  `DrainStatistics -> dcomp -> NtFlipObjectQueryNextMessageToProducer ->
  dxgkrnl!CPushLock::AcquireLockExclusive`.
- Frame 993: 3.0055 ms and 1.9891 ms off CPU inside
  `ComPtr<ID3D11Texture2D>::Reset -> D3D11/vendor -> VidMmFreeGpuVirtualAddress ->
  QueueSystemCommandAndWaitInternal -> WaitForFences`.
- Frame 1470: 3.4003 ms off CPU inside Composition Present, through the same
  GPU virtual-address release / fence-wait chain.
- Frame 993: another 2.3212 ms off CPU inside Composition Present through that chain.

Off-CPU intervals include any ready-to-running delay; this helper does not split it.
System and application symbols were resolved using the captured PDB and existing
matching local Microsoft symbol cache. Vendor and some dcomp frames remain unnamed.
These stacks identify blocking call paths, not the fence producer, driver culpability,
or a complete explanation of perceived stutter.

## Artifacts and next decision

Ignored analysis artifacts beside the raw capture: `feedback-details.json`,
`etl-targets.tsv`, `ab-wait-stacks.tsv`, `analysis-ab-waits.json`, and `system-trace.etlx`.
Raw capture files are unchanged. Helpers select only complete RenderFrame intervals
that contain completed detached resize events.

The evidence supports investigating buffer lifetime and the synchronous feedback
collection path separately. It does not establish that throttling statistics, deferring
release or changing buffer reuse would be correct or improve UX. Keep production policy
unchanged and retain the failed B outcome. No immediate repeat capture is required merely
to confirm these already observed call paths; any future capture needs a recording window
that avoids consuming the fixed file budget during setup and idle frames.
