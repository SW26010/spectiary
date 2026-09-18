# Explicit project identity and naming contracts

Status: Accepted. Date: 2026-09-18. Issues: #106, #107, #103.

The selected public name is Spectiary. `config/project_identity.json` owns
independent values for product display, LocalAppData leaf, artifact basename,
sidecar filename and shell identity. CMake generates the C++ contract; packaging
reads the same JSON. None of these values is derived from display text.

The immutable application identity is the full founding token
`0238d5bf7b34bb99c006f9807537d31234ca2e3d`. It is neither a runtime Git lookup nor
a shortened UUID. Current APIs accept strings, so no GUID representation is
needed. IPC and shared lock/digest namespaces use this root with semantic
suffixes. Local UI identifiers and layout keys use neutral semantic names.

Build metadata schema 6 separates `application_id`, `product.name`,
`product.version`, `artifact.file`, build provenance and optional deployment.
Only schema 6 is accepted. Old filenames are not discovery aliases. Product
display text is required descriptive metadata, not an identity match key.
Artifact path validation, executable SHA-256 binding during finalization and
packaging, atomic publication and constrained failure cleanup remain required.
Runtime deployment parsing remains independent of provenance availability.

The explicit LocalAppData leaf is `Spectiary`. #103 owns the final lowercase
config/state/logs/unsaved child layout and physical migration policy. This
foundation does not declare #106 complete: the rename playbook must be updated
as #103 settles that layout. Pre-release local caches/layout may reset; canonical
user documents must never be silently rewritten as a side effect of a rename.

Source namespaces, source filenames and CMake target names are implementation
symbols and need not track product branding. They are not discovery, storage,
wire-format or machine identity values.
