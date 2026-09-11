# Bounded incremental buffer replacement proposal

> Historical investigation record. See the [current resize investigation](../README.md) for current decisions and next steps.

Status: design only. No resize/presentation implementation is changed by this document.
The subsequent [default-off diagnostic implementation](../experiments/incremental-buffers.md)
records the implemented scope and remaining validation requirements.
The feedback-frequency experiment failed its UX test; see
[paired evidence](../evidence/20260911-feedback-ab.md).

## Objective and independent variable

Test whether replacing at most one slot per participating frame reduces the long
synchronous releases observed when all three slots are reset on every size change.
Keep feedback frequency at its ordinary default and keep the window-style arm fixed
within the pair. Neither earlier experiment is implicitly promoted to production.

The following would be a new resize policy requiring an explicit implementation
decision. It changes the timing of buffer allocation/release and can skip a resize
frame when no suitable buffer is available; it must not be hidden as instrumentation.

## State and transitions

- Per viewport: latest requested width/height and request serial; monotonically
  increasing allocation generation; three owned slots. Each slot records dimensions,
  generation, creation result and whether it is the most recently submitted slot.
- Native/renderer resize updates only the latest requested dimensions. Intermediate
  requests are superseded, not queued. The native callback still receives every size.
- At acquisition, prefer an available slot whose dimensions exactly match the latest
  request. Otherwise attempt one replacement of an available, non-bound old slot.
  If no such slot is available, follow the existing nonblocking skip outcome; do not
  draw into an old-size buffer under a new-size source rectangle.
- Build the replacement into one temporary slot. Validate texture, RTV, presentation
  buffer and available-event creation before installing it. A failed partial build
  releases only its temporary resources and leaves the old three slots intact.
- After a successful replacement, release the superseded slot in the same defined
  resource order. Select only an available exact-size buffer, set its exact source
  rectangle, draw and Present. Publish the bound generation only after successful
  submission. Failure paths must account for any pending SetBuffer/SetSourceRect state.
- Continue replacing old dimensions over later frames if needed. New resize requests
  replace the desired dimensions immediately; never retain an unbounded list of sizes.
- Destroy, device loss and fallback cancel the desired request and release all owned
  slots once. No worker thread owns presentation objects in this first experiment.

These are proposed invariants, not claims that available-event signaling guarantees
cheap Release or GPU allocation retirement. A bound-buffer exclusion also needs
validation against presentation-surface ownership; an application slot ID alone does
not describe every system-held reference.

## Memory and work limits

Proposed experimental limits: three installed slots plus one temporary replacement,
at most one replacement attempt per viewport/frame, and a 256 MiB checked logical
RGBA8 pixel budget across installed and temporary textures. Use overflow-checked
`width * height * 4`, include the old slot until replacement commits, and reject a
request exceeding the budget before allocation. This logical budget does not bound
driver metadata, alignment or internal retained resources; separately measure process
GPU memory and system-resource trends during continuous oscillation.

An over-budget request or unrecoverable replacement failure should take the explicit
existing DXGI fallback path with a distinct reason, not silently exceed the cap, spin,
or repeatedly allocate every frame. The 256 MiB value is an experimental proposal,
not a product requirement. Large-display behavior must be reviewed before adoption.

## Machine-checkable acceptance before interactive evidence

1. Rapid size oscillation never queues more than the latest request; generations do
   not alias across viewport destruction/recreation. Two viewports progress independently.
2. Never render into or retire an unavailable/bound candidate under the proposed rules.
   No available candidate produces a skip, with no allocation and no blocking wait added.
3. At most one replacement attempt per frame; owned logical memory includes temporary
   and installed resources, with integer-overflow and over-budget cases covered.
4. Failure at each creation step preserves the old generation and records the failed
   operation. Fallback and shutdown do not double-close handles or reuse destroyed slots.
5. Source rectangle, bound texture dimensions and selected generation agree for every
   successful Present; no stale request can commit after a newer request supersedes it.
6. Native window ownership/style, main viewport behavior, Present mode, feedback
   frequency and ordinary available-wait timeout remain the control settings.

Telemetry must distinguish `resize_request_serial`, allocation generation, slot,
bound generation, replacement attempt/commit/failure, live logical bytes, and skip
reason. Existing slot 0..2 identity alone cannot verify this lifecycle.

## Evaluation and stopping rule

Use a complete manually armed capture after setup, with the same source and display
conditions in control/treatment. Assess user-perceived stutter, full-frame tails,
resource Release and Present duration, skipped frames and memory behavior together.
Do not accept moving a wait from Resize to acquisition or Present as improvement.
If UX does not improve, keep the experiment off and retain the evidence. No additional
manual evidence is requested until implementation and the above checks are complete.
