# SpecForge sample-labeling schema 2.0.0 ASDF spike

This directory contains the reproducible experiments for GitHub issue #77. It
is deliberately separate from the production labeling persistence path. The
approved semantic model remains owned by issue #74.

The reference oracle is Python `asdf` 5.3.1 with `asdf-standard` 1.5.0. The
documents use SpecForge sample-labeling schema `2.0.0`, ASDF file format
`1.0.0`, and ASDF Standard `1.5.0`; these are three separate version axes. For
ad-hoc experiments, install the pinned environment into an ignored build
directory:

```powershell
python -m pip install --target build/asdf-labeling-spike-python -r tools/asdf_labeling_spike/requirements.txt
$env:PYTHONPATH = (Resolve-Path build/asdf-labeling-spike-python).Path
```

The checked profile is the canonical SpecForge sample-labeling schema `2.0.0`
document: task IDs are
lowercase UUID v4 values; task timestamps use fixed millisecond UTC text; task
origin, optional description, and optional authors round-trip through the Python
and native readers/writers; empty authors remain absent, and `annotation.name`
is rejected. The corpus exercises a deterministic `head` build source with a
full 40-character lowercase hexadecimal revision and `working_tree` with the
revision absent; production generations use the build identity compiled into
their writer. Every annotation declares the fixed schema-2 alignment contract
`mode: by_index` against `target: sample_roster`; alignment-less or differently
aligned schema-2 documents are rejected. Promoted annotation provenance names must be portable basenames:
`.`/`..`, path separators, and ASCII drive prefixes are rejected by both
oracles. The native reader
also enforces real calendar dates, origin shape/format, and exact optional
`sha256:` annotation fingerprints.

Generate and validate the checked-in golden corpus:

```powershell
python tools/asdf_labeling_spike/asdf_spike.py generate --output tests/fixtures/asdf_labeling
python tools/asdf_labeling_spike/asdf_spike.py validate --fixtures tests/fixtures/asdf_labeling
```

Run the representation, wire-version, missing-value, and autosave experiments:

```powershell
python tools/asdf_labeling_spike/asdf_spike.py experiments --output build/asdf-labeling-spike-results
```

Run the second-round canonical-format and realistic-edit matrix requested by
the follow-up on issue #77:

```powershell
python tools/asdf_labeling_spike/asdf_spike.py second-round --output build/asdf-labeling-spike-results
```

This runs 96 isolated cases over 10k, 100k, and 1M samples. It compares compact
UTF-8 JSON, whole-document JSON + zlib, ASDF UCS-4/int32 blocks, and ASDF +
zlib at levels 1, 6, and 9 for ASCII/Unicode rosters and mostly-labeled/
mostly-unlabeled values. Each case measures initial write, hydration, one-label
and 1000-label edits, full atomic persistence, and (for ASDF) atomic persistence
that copies the immutable encoded roster block verbatim and re-encodes only the
values block. `foreground_edit` is only the synchronous in-memory mutation;
the separately reported persistence time is work eligible for a debounced
background autosave. The command writes `second-round-benchmark.json`.

Build the native subset with the repository's required MSVC wrapper, then run
the compressed read/write/rewrite matrix:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/build-ninja-msvc-debug.ps1 `
  -Configure -Preset ninja-msvc-debug-asdf-labeling-spike
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/build-ninja-msvc-debug.ps1 `
  -Preset ninja-msvc-debug-asdf-labeling-spike -Target specforge_asdf_labeling_spike_native
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/build-ninja-msvc-debug.ps1 `
  -Configure
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/build-ninja-msvc-debug.ps1 `
  -Target specforge_sample_labeling_asdf_codec_tests
python tools/asdf_labeling_spike/asdf_spike.py interoperability `
  --fixtures tests/fixtures/asdf_labeling `
  --native build/ninja-msvc-debug-asdf-labeling-spike/tools/asdf_labeling_spike/specforge_asdf_labeling_spike_native.exe `
  --production-native build/ninja-msvc-debug/specforge_sample_labeling_asdf_codec_tests.exe `
  --output build/asdf-labeling-spike-results/interoperability.json
```

The required CTest/CI route provisions the oracle in the dedicated preset's
build tree and runs both native executables from that tree:

```powershell
python -m pip install --target build/ninja-msvc-debug-asdf-labeling-spike/asdf-labeling-spike-python `
  -r tools/asdf_labeling_spike/requirements.txt
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/build-ninja-msvc-debug.ps1 `
  -Configure -Preset ninja-msvc-debug-asdf-labeling-spike
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/build-ninja-msvc-debug.ps1 `
  -Preset ninja-msvc-debug-asdf-labeling-spike `
  -Target specforge_asdf_labeling_interoperability_targets
ctest --test-dir build/ninja-msvc-debug-asdf-labeling-spike `
  -R '^specforge_asdf_labeling_interoperability$' --output-on-failure
```

The matrix proves Python zlib writer -> native reader, native level-6 zlib
writer -> Python reader, and Python zlib writer -> native label-only rewrite ->
both readers. It also runs the production writer's complete YAML-special scalar
matrix through the pinned official ASDF 5.3.1 reader; opens explicit-roster and
source-index production writes and rewrites; and rewrites Python-origin inputs
through the production component. The Python-written `forward_unknown` fixture
also passes through the production metadata-changing rewrite; ASDF 5.3.1 checks
the edited known semantics, retained future metadata, and ambiguous scalar
runtime types without string coercion. Every branch
is compared using complete canonical semantics. The explicit-roster path
additionally verifies that the
encoded Unicode roster block is byte-for-byte unchanged while the values block
is replaced. A differential duplicate-key case confirms that official ASDF
selects the last value while the production reader/rewrite rejects the
ambiguous document before creating output. The native CLI also exposes the
historical raw-block experiment directly for an already validated SpecForge
sample-labeling schema `2.0.0` input document:

```powershell
build/ninja-msvc-debug-asdf-labeling-spike/tools/asdf_labeling_spike/specforge_asdf_labeling_spike_native.exe `
  rewrite-value input.asdf output.asdf 0 1
```

That experimental command first validates the complete known schema and then
copies the YAML tree prefix byte-for-byte while replacing the values block, so
unknown root, origin, and origin-annotation fields retain their representation.
It is not the production lifecycle save path and intentionally does not model
the controller clock; production semantic saves use the full metadata-aware
document rewrite.

Both benchmark commands use a subprocess per case so reported peak RSS is not
contaminated by allocations retained by earlier cases. Generated benchmark
files are deleted after each case; the JSON evidence is retained.
