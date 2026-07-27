# Sample Labeling Architecture

SpecForge models per-sample data as sample annotation results and treats storage
formats such as NPY as annotation I/O adapters, not as the domain model. Sample
labeling task records, workflow settings, autosave state, and output paths live
in local user state, while exported label result files stay compact and
format-specific. Portable sample label result metadata may live beside an
exported compact result so the label codes, display names, shortcuts, unlabeled
sentinel, and expected result shape travel with the data product. Annotation I/O
belongs behind a domain or service boundary; UI code consumes loaded annotation
results and save state rather than parsing dtype, shape, or file write
capabilities.

`SampleLabelingController` is the single mutable owner of active tasks, drafts,
output-save state, retry scheduling, and the labeling state cache. Callers
receive a borrowed read-only view with a revision and submit atomic domain
operations. View pointers are valid only until the controller's next mutation
or destruction and must not be retained across command submission or
maintenance. Operation results report the resulting revision and output/state
persistence status, including rejected operations. `SampleWorkflowCoordinator`
still owns navigation order, filters, sorting, and undo coordination. A
labeling write may request advance, but only the coordinator resolves and
applies that navigation request.

Workflow mutations cross the coordinator/session boundary as one complete
transition outcome. The outcome carries domain change flags, navigation
results, any snapshot index that must be loaded next, command-specific status,
and whether the session view must be invalidated. `SourceCollectionSession`
consumes that outcome while retaining ownership of the source roster,
activation, background follow-up, and presentation lifecycles.

We keep sample navigation, sample filtering, sample annotation inspection, and
active manual labeling as separate UI surfaces. This avoids letting a labeling
window own unrelated concerns like current index, sample-filter composition, loaded
read-only annotations, or format-specific file matching.

Rejected alternatives were storing local task records, drafts, pending values,
active task state, sample filters, or other workflow recovery state inside or
beside every label array, making `*_y.npy` the default output semantics, and using one
labeling window to both inspect every annotation and edit the active task. Those
choices would couple workflow recovery to one file format, create conflicting
sources of truth, and make shortcut ownership and write safety harder to reason
about.

## Local persistence failure semantics

Source-session, navigation, labeling, and workflow state remain four
independently validated, independently written versioned JSON caches. Their
codecs continue to own schema support and field validation; the session only
aggregates owner-reported health.

| Condition | User signal | Continue? | Clear condition |
| --- | --- | --- | --- |
| Missing cache | None; use defaults | Yes | Not applicable |
| Corrupt cache | Non-blocking owner warning | Yes, with defaults | That owner successfully writes valid replacement state |
| Unsupported schema | Non-blocking owner warning | Yes, without reading unsupported state | That owner successfully writes state after a user mutation |
| Partial save | Overall `retrying` health naming each failed owner while retries remain scheduled | Yes; attempt every other dirty cache | Each failed owner succeeds independently |
| Retrying | Overall `retrying` health; existing retry deadline remains active | Yes | First successful retry |
| Recovered | Overall recovery health | Yes | Next user mutation owned by the recovered cache |

There is no cross-file transaction: a failure in one cache must not suppress
attempts for the other caches. Normal shutdown consumes a per-owner flush
result so an incomplete flush is not reduced to an ignored aggregate boolean.
