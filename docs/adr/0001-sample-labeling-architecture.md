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
