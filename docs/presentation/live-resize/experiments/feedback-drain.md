# Acquisition-only feedback diagnostic

> Historical investigation record. See the [current resize investigation](../README.md) for current decisions and next steps.

Outcome: [the paired trial](../evidence/20260911-feedback-ab.md) did not
improve user experience. Keep this diagnostic disabled; the commands below are
historical reproduction instructions, not a request for another trial. Both captures
hit the file bound, so capture-window control must be corrected before requesting
further interactive evidence.

Implemented as a default-off diagnostic after the
[call-site review](../design/feedback-and-buffer-review.md).

Only `spectiary_redirection_experiment` reads
`SPECTIARY_EXPERIMENT_FEEDBACK_ACQUIRE_ONLY=1`. The ordinary executable does not
activate the option. The runtime emits `feedback_experiment.acquire_only`, and
the runner records the requested state beside the executable hash.

When enabled, detached renderer collection calls use `TakeCompositionFeedback(false)`.
This still takes transitions and accumulated feedback through the existing thresholds
and error reporting, but does not drain. The existing BeginFrame drain is unchanged,
including its position before buffer availability testing, zero-timeout statistics
polling and complete queue-drain loop. A normal ImGui frame invokes one acquisition
per participating viewport, so the four drain sites reduce to one. There is no new
global frame cache or cross-viewport suppression. If callbacks unexpectedly acquire
twice in a frame, the analyzer rejects that capture rather than hiding the duplicate.

Main-window calls retain the default `drain=true`. Destroy/recreate has no cached drain
state to inherit; renderer shutdown also clears the experiment flag. DXGI has no
Composition statistics queue. This change adds no alternate handling of acquisition
skip, fallback, failed statistics, buffer release, size, Present flags or pacing.
Feedback discovered after acquisition may be reported on the next participating frame.
An inactive or destroyed viewport may leave final statistics unread; this diagnostic
does not promise identical final feedback counts or solve idle queue servicing.

Validation covers real detached creation, resize, draw/Present and two successive
lifetimes: creation/resize collection performs no drain, while a Composition frame
performs exactly one drain under viewport acquisition. Existing baseline tests remain.
Schema tests check activation and reject a drain at a collection site. The analyzer
also rejects duplicate detached drains in the same viewport lifetime/frame. Existing
full-capture, event-parent, HRESULT and zero-timeout checks remain mandatory.

To compare a single changed factor, hold the existing B window-style setting fixed
and use the same newly built executable for both runs:

```powershell
# Control: existing statistics collection behavior, B window style.
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/profile-live-resize.ps1 -Scenario NativeSize -RedirectionArm B -FeedbackBreakdown
# Treatment: only feedback collection frequency changes.
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/profile-live-resize.ps1 -Scenario NativeSize -RedirectionArm B -FeedbackBreakdown -FeedbackAcquireOnly
```

The runner now starts with recording off for these commands. Prepare Spectrum,
then use Settings > Diagnostics > Start Recording and drag its outer border during
the five-second capture; close normally after recording finishes. Avoid loading a new dataset between
the two arms. These initial UX trials omit WPR because the blocking paths already
have system evidence; add `-SystemTrace` only for a later targeted correlation.
The fixed 100 MB cap remains; a cap-terminated recording is invalid and must not
be presented as successful validation. The previous truncated capture cannot serve
as the quantitative control for this build.

Judge whole-frame timing and user experience, not the now-shorter resize collection
span alone. Queue work may move to acquisition and fence waits in release/Present
remain independent. B is still an unsuccessful UX experiment; this diagnostic does
not promote its window style to production. A beneficial B result would still need
verification with the ordinary window style before any production decision.
